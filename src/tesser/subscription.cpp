// Change tracking, subscriptions and events (docs/DESIGN.md section 11).
#include <string.h>

#include <algorithm>

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/call.h"
#include "tesser/detail/lazy_patch.h"
#include "tesser/remote.h"

namespace tesser {

namespace {

constexpr uint32_t kNotifyBackoffMs = 50;      // a notification put off, first wait
constexpr uint32_t kNotifyBackoffMaxMs = 2000;  // and longest

// Registry of live APIs, so an EventNode can find the tree it belongs to.
// Registration is a lock-free push: APIs are often globals, constructed
// before any mutex in another translation unit is guaranteed to exist.
std::atomic<Api*> g_apis{nullptr};

Mutex& apisMutex() { return detail::treeWalkMutex(); }

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

void changesIn(const Object& o, uint32_t since, uint32_t now, int depth, const Subscription* top,
               const detail::RenderOptions& options, detail::LazyPatch& lp, JsonWriter& w) {
    if (depth <= 0) return;
    for (const Node* c = o.first(); c; c = c->next()) {
        if (!detail::hasValue(*c)) continue;
        if (c->type() == NodeType::Remote && !options.remotes) continue;
        if (c->readAccess() > options.access) continue;
        if (top && !passesFilter(*top, c->name())) continue;
        if (c->type() == NodeType::Object) {
            if (!lp.enter(c->name())) continue;
            changesIn(static_cast<const Object&>(*c), since, now, depth - 1, nullptr, options, lp, w);
            lp.leave();
        } else if (detail::isLeaf(*c) && changedSince(c->generation(), since, now)) {
            lp.key(c->name());
            detail::writeLeaf(w, *c, options);
        }
    }
}

// The level needed to read the node at `path`, its ancestors' included. A
// path leaving the local tree (below a remote node) stops at its last local
// node.
Access readNeed(const Object& root, std::string_view path) {
    Access need = root.readAccess();
    const Node* node = &root;
    size_t pos = 0;
    while (pos < path.size()) {
        if (path[pos] == '/') {
            pos++;
            continue;
        }
        size_t slash = path.find('/', pos);
        if (slash == std::string_view::npos) slash = path.size();
        if (node->type() != NodeType::Object) return need;
        node = static_cast<const Object*>(node)->child(path.substr(pos, slash - pos));
        if (!node) return need;
        if (node->readAccess() > need) need = node->readAccess();
        pos = slash;
    }
    return need;
}

// Appends the closing of a notification envelope: the level of a
// subscription made with one ("sub@user"), so the gateway that made it
// knows which of its subscriptions it is for.
void closeEnvelope(std::string& msg, bool overflow, Access ceiling) {
    if (ceiling < Access::Admin) {
        msg += ",\"as\":\"";
        msg += toString(ceiling);
        msg += '"';
    }
    if (overflow) msg += ",\"overflow\":true";
    msg += '}';
}

}  // namespace

// ---- registry ---------------------------------------------------------------

Api::Api() : Object("") {
    remoteHooks_.reset(new RemoteHooks());
    Api* head = g_apis.load();
    do {
        nextApi_ = head;
    } while (!g_apis.compare_exchange_weak(head, this));
}

Api::~Api() {
    {
        MutexGuard guard(mutex_);
        for (Node* n : retired_) delete n;
        retired_.clear();
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
    // Without any API's lock: trees only grow, by appends published whole
    // (Node::next_), so a walk sees a consistent tree. A caller holding its
    // own API's lock (an event's first emit()) mustn't take another's.
    MutexGuard guard(apisMutex());
    for (Api* a = g_apis.load(); a; a = a->nextApi_) {
        path.clear();
        if (&node == a) return a;
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

size_t Api::removeSubscriptions(Subscriber* subscriber, std::string_view path, Access ceiling) {
    MutexGuard guard(mutex_);
    std::vector<Subscription> gone;
    for (size_t i = 0; i < subs_.size();) {
        if (subs_[i].subscriber == subscriber && subs_[i].path == path && subs_[i].ceiling == ceiling) {
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
        // Upstream subscriptions are per path and level.
        bool used = false;
        for (const Subscription& s : subs_) {
            used = used || (s.forwarded && s.node == g.node && s.path == g.path && s.access == g.access);
        }
        if (used) {
            // The others may need less: the upstream subscription narrows.
            static_cast<RemoteNode*>(g.node)->narrowed(g.path, g.access);
        } else {
            static_cast<RemoteNode*>(g.node)->released(g.path, g.access);
        }
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

void Api::tokensChanged() {
    MutexGuard guard(mutex_);
    tokenGeneration_++;
    // Each client's level, asked once; admin for those whose level doesn't
    // come from the check.
    std::vector<std::pair<Subscriber*, Access>> levels;
    auto levelOf = [&levels](Subscriber* s) {
        for (const auto& l : levels) {
            if (l.first == s) return l.second;
        }
        return Access::Admin;
    };
    for (const Subscription& s : subs_) {
        bool seen = false;
        for (const auto& l : levels) seen = seen || l.first == s.subscriber;
        Access level = Access::Admin;
        if (!seen && s.subscriber->recheckLevel(level)) levels.emplace_back(s.subscriber, level);
    }
    // Subscriptions keep the level they were made at: those above the new
    // one end, as when a line signs out.
    std::vector<Subscription> gone;
    std::vector<Subscriber*> lowered;
    for (size_t i = 0; i < subs_.size();) {
        Subscriber* s = subs_[i].subscriber;
        if (subs_[i].access <= levelOf(s)) {
            i++;
            continue;
        }
        if (std::find(lowered.begin(), lowered.end(), s) == lowered.end()) lowered.push_back(s);
        gone.push_back(std::move(subs_[i]));
        subs_.erase(subs_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    subscriptionsRemoved(gone, false);
    for (Subscriber* s : lowered) s->levelLowered();
}

void Api::dropSubscriber(Subscriber* subscriber) {
    {
        // Queued requests from this client can't be answered any more. One
        // that runQueued() took out already runs under the API lock, taken
        // below: it is over before the client can be destroyed.
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

size_t Api::subscriptionsOf(const Subscriber* subscriber) const {
    MutexGuard guard(mutex_);
    size_t n = 0;
    for (const Subscription& s : subs_) n += s.subscriber == subscriber;
    return n;
}

bool Api::subscribedLocked(const Node& node) const {
    for (const Subscription& s : subs_) {
        if (s.node == &node) return true;
    }
    return false;
}

void Api::retire(Node* node) {
    if (yielded_) {
        retired_.push_back(node);
    } else {
        delete node;
    }
}

void Api::poll() { poll(millis32()); }

void Api::poll(uint32_t nowMs) {
    runQueued();
    MutexGuard guard(mutex_);
    if (!yielded_ && !retired_.empty()) {
        for (Node* n : retired_) delete n;
        retired_.clear();
    }
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
    if (!sub.changes) return;  // events only
    if (sub.interval && nowMs - sub.lastFlushMs < sub.interval) return;
    if (sub.backoffMs && static_cast<int32_t>(nowMs - sub.retryAtMs) < 0) return;
    uint32_t gen = currentGeneration();
    if (gen == sub.since) return;
    if (gen - sub.since > kGenerationHorizon) {
        // Undelivered for long: the window is narrowed, so stamps a lap old
        // can't fall into it. What is older is sent no more, and "overflow"
        // tells the client to re-read.
        sub.since = gen - kGenerationHorizon;
        sub.subscriber->missed = true;
    }

    // Put off: `since` stays, so the changes go with a later flush, merged
    // with newer ones; not at the next poll() but after a wait that grows,
    // rather than build the same notification every few milliseconds
    // while it can't go (docs/DESIGN.md section 15).
    auto putOff = [&] {
        sub.backoffMs = sub.backoffMs ? std::min<uint32_t>(sub.backoffMs * 2, kNotifyBackoffMaxMs) : kNotifyBackoffMs;
        sub.retryAtMs = nowMs + sub.backoffMs;
    };
    // Under the floor, even a small one waits: its allocations, unchecked
    // under 256 bytes, are what aborts on a heap taken by something else.
    if (!heapAbove(0)) {
        putOff();
        return;
    }

    // Rendered straight into the envelope (no separate body to copy), and
    // refused, to be tried again at the next flush, if the heap can't hold
    // it: an abort would take the device down.
    std::string msg;
    {
        StringSink head(msg, 0, true);
        JsonWriter m(head);
        m.beginObject();
        m.key("op");
        m.string("change");
        m.key("path");
        m.string(sub.path.empty() ? std::string_view("/") : std::string_view(sub.path));
        m.key("body");
        if (head.lowMemory) {
            putOff();
            return;
        }
    }
    StringSink sink(msg, 0, true);
    JsonWriter w(sink);
    bool any = false;
    const Node& n = *sub.node;
    detail::RenderOptions options;
    options.remotes = sub.remotes;
    options.access = sub.access;
    if (n.type() == NodeType::Object) {
        detail::LazyPatch lp(w);
        int depth = sub.depth < 0 || sub.depth > config_.maxDepth ? config_.maxDepth : sub.depth;
        changesIn(static_cast<const Object&>(n), sub.since, gen, depth, &sub, options, lp, w);
        any = lp.finish();
    } else if (detail::isLeaf(n) && changedSince(n.generation(), sub.since, gen)) {
        detail::writeLeaf(w, n, options);
        any = true;
    }
    if (sink.lowMemory) {
        putOff();
        return;
    }
    if (!any) {
        sub.since = gen;
        return;
    }
    // Room for the envelope's end, through the accountant: a string the
    // sink grew to its exact size would otherwise double for one more byte
    // (libstdc++), unchecked.
    if (!growFor(msg, msg.size() + 48)) {
        putOff();
        return;
    }
    bool overflow = sub.subscriber->missed.exchange(false);
    closeEnvelope(msg, overflow, sub.ceiling);
    if (sub.subscriber->notify(msg, Delivery::Reliable)) {
        sub.since = gen;
        sub.lastFlushMs = nowMs;
        sub.backoffMs = 0;
    } else {
        // Not advancing `since` makes a later flush resend these changes,
        // merged with newer ones.
        sub.subscriber->missed = true;
        putOff();
    }
}

// ---- events -----------------------------------------------------------------

void Api::emitEvent(const std::string& path, const std::function<void(JsonWriter&)>& write,
                    const std::function<bool(const Subscription&)>& accept) {
    MutexGuard guard(mutex_);
    if (subs_.empty()) return;
    // Under the floor, not even a small event goes: its allocations
    // (buffers under 256 bytes, the shared message, the lists below) would
    // live on what is left, which the network stack can take whole in a
    // burst. The subscribers it was for are marked missed.
    bool room = heapAbove(0);

    std::string base;
    StringSink sink(base, 0, true);  // guarded: refused on a short heap
    JsonWriter w(sink);
    bool built = false;
    if (room) {
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
        built = w.ok();
    }

    Access need = readNeed(*this, path);

    // One message per variant (level, overflow), shared by every subscriber
    // it goes to, with its bytes counted while any of them holds it.
    struct Held {
        std::string text;
        Reservation bytes;
    };
    struct Variant {
        Access ceiling;
        bool overflow;
        std::shared_ptr<const std::string> message;
    };
    std::vector<Variant> variants;
    auto messageFor = [&](Access ceiling, bool overflow) -> std::shared_ptr<const std::string> {
        for (const Variant& v : variants) {
            if (v.ceiling == ceiling && v.overflow == overflow) return v.message;
        }
        if (!built) return nullptr;
        auto held = std::make_shared<Held>();
        if (!growFor(held->text, base.size() + 48)) return nullptr;
        held->text = base;
        closeEnvelope(held->text, overflow, ceiling);
        held->bytes = memoryBudget().reserve(held->text.capacity(), 0, false, false);
        std::shared_ptr<const std::string> m(held, &held->text);
        variants.push_back(Variant{ceiling, overflow, m});
        return m;
    };

    // Once per client and level: a gateway holding subscriptions at two
    // levels gets the event for each.
    std::vector<std::pair<Subscriber*, Access>> notified;
    for (const Subscription& sub : subs_) {
        if (!sub.events || sub.waiting || sub.access < need) continue;
        if (accept && !accept(sub)) continue;
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
        if (!room) {
            sub.subscriber->missed = true;
            continue;
        }
        bool seen = false;
        for (const auto& n : notified) seen = seen || (n.first == sub.subscriber && n.second == sub.ceiling);
        if (seen) continue;
        notified.emplace_back(sub.subscriber, sub.ceiling);

        bool overflow = sub.subscriber->missed.exchange(false);
        std::shared_ptr<const std::string> msg = messageFor(sub.ceiling, overflow);
        if (!msg || !sub.subscriber->notifyShared(msg, Delivery::Reliable)) sub.subscriber->missed = true;
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
