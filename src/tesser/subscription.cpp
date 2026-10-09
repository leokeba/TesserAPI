// Change tracking, subscriptions and events (docs/DESIGN.md section 11).
#include <string.h>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/detail/lazy_patch.h"
#include "tesser/remote.h"

namespace tesser {

namespace {

// Registry of live APIs, so an EventNode can find the tree it belongs to.
// Registration is a lock-free push: APIs are often globals, constructed
// before any mutex in another translation unit is guaranteed to exist.
std::atomic<Api*> g_apis{nullptr};

Mutex& apisMutex() {
    static Mutex m;  // constructed on first use
    return m;
}

bool findPath(const Object& o, const Node* target, std::string& path) {
    for (const Node* c = o.first(); c; c = c->next()) {
        size_t len = path.size();
        path += '/';
        path += c->name();
        if (c == target) return true;
        if (c->type() == NodeType::Object && findPath(static_cast<const Object&>(*c), target, path)) return true;
        path.resize(len);
    }
    return false;
}

bool inList(std::string_view list, std::string_view name) {
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string_view::npos) comma = list.size();
        std::string_view item = list.substr(pos, comma - pos);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (item == name) return true;
        pos = comma + 1;
    }
    return false;
}

bool passesFilter(const Subscription& sub, std::string_view name) {
    if (!sub.keys.empty() && !inList(sub.keys, name)) return false;
    if (!sub.exclude.empty() && inList(sub.exclude, name)) return false;
    return true;
}

void changesIn(const Object& o, uint16_t since, int depth, const Subscription* top, bool remotes, detail::LazyPatch& lp,
               JsonWriter& w) {
    if (depth <= 0) return;
    for (const Node* c = o.first(); c; c = c->next()) {
        if (c->type() == NodeType::Action || c->type() == NodeType::Event) continue;
        if (c->type() == NodeType::Remote && !remotes) continue;
        if (top && !passesFilter(*top, c->name())) continue;
        if (c->type() == NodeType::Object) {
            if (!lp.enter(c->name())) continue;
            changesIn(static_cast<const Object&>(*c), since, depth - 1, nullptr, remotes, lp, w);
            lp.leave();
        } else if (detail::isLeaf(*c) && newerGeneration(c->generation(), since)) {
            lp.key(c->name());
            detail::writeLeaf(w, *c, remotes);
        }
    }
}

// Appends the closing of a notification envelope.
void closeEnvelope(std::string& msg, bool overflow) {
    if (overflow) msg += ",\"overflow\":true";
    msg += '}';
}

}  // namespace

// ---- registry ---------------------------------------------------------------

Api::Api() : Object("") {
    Api* head = g_apis.load();
    do {
        nextApi_ = head;
    } while (!g_apis.compare_exchange_weak(head, this));
}

Api::~Api() {
    {
        MutexGuard guard(mutex_);
        for (Subscription& s : subs_) {
            if (s.waiting) s.waiting->release();
        }
        subs_.clear();
    }
    {
        MutexGuard guard(queueMutex_);
        for (QueuedRequest* q : queue_) {
            q->reply->release();
            delete q;
        }
        queue_.clear();
    }
    MutexGuard guard(apisMutex());
    Api* self = this;
    if (g_apis.compare_exchange_strong(self, nextApi_)) return;
    // Not the head (a push may have happened meanwhile): unlink in place.
    for (Api* p = g_apis.load(); p; p = p->nextApi_) {
        if (p->nextApi_ == this) {
            p->nextApi_ = nextApi_;
            break;
        }
    }
}

Api* Api::owner(const Node& node, std::string& path) {
    MutexGuard guard(apisMutex());
    for (Api* a = g_apis.load(); a; a = a->nextApi_) {
        path.clear();
        if (&node == a) return a;
        MutexGuard apiGuard(a->mutex_);
        if (findPath(*a, &node, path)) return a;
    }
    path.clear();
    return nullptr;
}

bool Api::changed(std::string_view path) {
    MutexGuard guard(mutex_);
    Node* node = this;
    size_t pos = 0;
    while (pos < path.size()) {
        if (path[pos] == '/') {
            pos++;
            continue;
        }
        size_t slash = path.find('/', pos);
        if (slash == std::string_view::npos) slash = path.size();
        if (node->type() != NodeType::Object) return false;
        node = static_cast<Object*>(node)->child(path.substr(pos, slash - pos));
        if (!node) return false;
        pos = slash;
    }
    node->changed();
    return true;
}

// ---- subscriptions ----------------------------------------------------------

Status Api::subscriptionAllowed(Subscriber* subscriber) const {
    MutexGuard guard(mutex_);
    if (subs_.size() >= config_.maxSubscriptions) return Status::Busy;
    size_t mine = 0;
    for (const Subscription& s : subs_) {
        if (s.subscriber == subscriber) mine++;
    }
    return mine >= config_.maxSubscriptionsPerClient ? Status::Busy : Status::Ok;
}

void Api::addSubscription(Subscription&& sub) {
    MutexGuard guard(mutex_);
    sub.since = currentGeneration();
    subs_.push_back(std::move(sub));
}

size_t Api::removeSubscriptions(Subscriber* subscriber, std::string_view path) {
    MutexGuard guard(mutex_);
    std::vector<Subscription> gone;
    for (size_t i = 0; i < subs_.size();) {
        if (subs_[i].subscriber == subscriber && subs_[i].path == path) {
            gone.push_back(std::move(subs_[i]));
            subs_.erase(subs_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
    subscriptionsRemoved(gone, false);
    return gone.size();
}

void Api::subscriptionsRemoved(std::vector<Subscription>& gone, bool clientGone) {
    for (Subscription& g : gone) {
        if (g.waiting) {
            if (!clientGone) writeError(*g.waiting, Status::NotFound, g.path, "unsubscribed before the remote answered");
            g.waiting->release();
            g.waiting = nullptr;
            pendingDone();
        }
    }
    for (const Subscription& g : gone) {
        if (!g.forwarded) continue;
        bool used = false;
        for (const Subscription& s : subs_) used = used || (s.node == g.node && s.path == g.path);
        if (!used) static_cast<RemoteNode*>(g.node)->released(g.path);
    }
}

void Api::forwardedSubscriptions(const Node* node, const std::function<bool(Subscription&)>& fn) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < subs_.size();) {
        if (subs_[i].forwarded && subs_[i].node == node && !fn(subs_[i])) {
            subs_.erase(subs_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
}

void Api::dropSubscriber(Subscriber* subscriber) {
    {
        // Queued requests from this client can't be answered any more.
        MutexGuard guard(queueMutex_);
        for (size_t i = 0; i < queue_.size();) {
            if (queue_[i]->request.subscriber == subscriber) {
                queue_[i]->reply->release();
                delete queue_[i];
                queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                i++;
            }
        }
    }
    MutexGuard guard(mutex_);
    std::vector<Subscription> gone;
    for (size_t i = 0; i < subs_.size();) {
        if (subs_[i].subscriber == subscriber) {
            gone.push_back(std::move(subs_[i]));
            subs_.erase(subs_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
    subscriptionsRemoved(gone, true);
}

size_t Api::subscriptionCount() const {
    MutexGuard guard(mutex_);
    return subs_.size();
}

void Api::poll() { poll(millis32()); }

void Api::poll(uint32_t nowMs) {
    runQueued();
    MutexGuard guard(mutex_);
    if (persistHook_) persistHook_(*this, nowMs);
    if (subs_.empty()) return;
    if (nowMs - lastWatchMs_ >= config_.watchIntervalMs) {
        lastWatchMs_ = nowMs;
        sampleWatched(*this);
    }
    for (Subscription& sub : subs_) {
        if (!sub.forwarded) flush(sub, nowMs);
    }
}

void Api::sampleWatched(Node& node) {
    if (node.type() == NodeType::Object) {
        for (Node* c = static_cast<Object&>(node).first(); c; c = c->next()) sampleWatched(*c);
        return;
    }
    if (node.type() != NodeType::Value || !node.watched() || !node.meta_) return;
    HashSink sink;
    JsonWriter w(sink);
    static_cast<ValueNode&>(node).write(w);
    NodeMeta& m = *node.meta_;
    if (m.hashed && m.hash != sink.hash) node.changed();
    m.hash = sink.hash;
    m.hashed = true;
}

void Api::flush(Subscription& sub, uint32_t nowMs) {
    if (sub.interval && nowMs - sub.lastFlushMs < sub.interval) return;
    uint16_t gen = currentGeneration();
    if (gen == sub.since) return;

    std::string body;
    StringSink sink(body);
    JsonWriter w(sink);
    bool any = false;
    const Node& n = *sub.node;
    if (n.type() == NodeType::Object) {
        detail::LazyPatch lp(w);
        int depth = sub.depth < 0 || sub.depth > config_.maxDepth ? config_.maxDepth : sub.depth;
        changesIn(static_cast<const Object&>(n), sub.since, depth, &sub, sub.remotes, lp, w);
        any = lp.finish();
    } else if (detail::isLeaf(n) && newerGeneration(n.generation(), sub.since)) {
        detail::writeLeaf(w, n);
        any = true;
    }
    if (!any) {
        sub.since = gen;
        return;
    }

    std::string msg;
    StringSink msgSink(msg);
    JsonWriter m(msgSink);
    m.beginObject();
    m.key("op");
    m.string("change");
    m.key("path");
    m.string(sub.path.empty() ? std::string_view("/") : std::string_view(sub.path));
    m.key("body");
    m.raw(body);
    bool overflow = sub.subscriber->missed.exchange(false);
    closeEnvelope(msg, overflow);
    if (sub.subscriber->notify(msg, Delivery::Reliable)) {
        sub.since = gen;
        sub.lastFlushMs = nowMs;
    } else {
        // Not advancing `since` makes the next flush resend these changes,
        // merged with newer ones.
        sub.subscriber->missed = true;
    }
}

// ---- events -----------------------------------------------------------------

void Api::emitEvent(const std::string& path, const std::function<void(JsonWriter&)>& write) {
    MutexGuard guard(mutex_);
    if (subs_.empty()) return;

    std::string base;
    StringSink sink(base);
    JsonWriter w(sink);
    w.beginObject();
    w.key("op");
    w.string("event");
    w.key("path");
    w.string(path);
    w.key("body");
    if (write) {
        write(w);
    } else {
        w.null();
    }

    std::vector<Subscriber*> notified;
    for (const Subscription& sub : subs_) {
        if (!sub.events || sub.waiting) continue;
        // The subscription's path must be a prefix of the event's, at a
        // segment boundary, within its depth and key filter.
        std::string_view ev(path);
        std::string_view sp(sub.path);
        if (ev.size() < sp.size() || ev.compare(0, sp.size(), sp) != 0) continue;
        std::string_view rest = ev.substr(sp.size());
        if (!rest.empty() && rest.front() != '/') continue;
        if (!rest.empty()) {
            rest.remove_prefix(1);
            size_t slash = rest.find('/');
            std::string_view first = rest.substr(0, slash);
            if (!passesFilter(sub, first)) continue;
            int segments = 1;
            for (char c : rest) segments += c == '/';
            if (sub.depth >= 0 && segments > sub.depth) continue;
        }
        bool seen = false;
        for (Subscriber* s : notified) seen = seen || s == sub.subscriber;
        if (seen) continue;
        notified.push_back(sub.subscriber);

        std::string msg = base;
        bool overflow = sub.subscriber->missed.exchange(false);
        closeEnvelope(msg, overflow);
        if (!sub.subscriber->notify(msg, Delivery::Reliable)) sub.subscriber->missed = true;
    }
}

void EventNode::emitWith(const std::function<void(JsonWriter&)>& write) {
    if (!api_) api_ = Api::owner(*this, path_);
    if (api_) api_->emitEvent(path_, write);
}

void EventNode::emit() { emitWith(nullptr); }

void EventNode::emit(JsonVariantConst payload) {
    emitWith([payload](JsonWriter& w) { w.variant(payload); });
}

}  // namespace tesser
