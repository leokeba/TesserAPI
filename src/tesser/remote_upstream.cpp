#include "tesser/detail/remote_upstream.h"

#include <algorithm>

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/call.h"
#include "tesser/detail/lazy_patch.h"
#include "tesser/detail/remote_json.h"
#include "tesser/envelope.h"
#include "tesser/remote.h"

namespace tesser {
namespace detail {

Api* RemoteContext::api() const { return node_->api_.load(); }
const std::string& RemoteContext::localPath() const { return node_->localPath_; }
bool RemoteContext::findLocalPath(std::string& out) const { return node_->localPath(out); }
void RemoteContext::adopt(Api& api, std::string_view localPath) const {
    node_->api_ = &api;
    if (node_->localPath_.empty()) node_->localPath_.assign(localPath.data(), localPath.size());
}
const std::string& RemoteContext::remotePath() const { return node_->remotePath_; }
std::string RemoteContext::remotePathOf(std::string_view rest) const { return node_->remotePathOf(rest); }
bool RemoteContext::route(RemoteRoute& out) const { return node_->route(out); }
bool RemoteContext::online() const { return node_->online(); }
uint32_t RemoteContext::timeoutMs() const { return node_->timeoutMs(); }
void RemoteContext::changed() const { node_->changed(); }
UpstreamCache& RemoteContext::upstream() const { return node_->upstream(); }

namespace {

// A copy rendered like the value view: objects below `depth` levels as {}.
void writeLimited(JsonWriter& w, JsonVariantConst v, int depth) {
    if (v.is<JsonObjectConst>()) {
        w.beginObject();
        if (depth > 0) {
            for (JsonPairConst kv : v.as<JsonObjectConst>()) {
                w.key(std::string_view(kv.key().c_str(), kv.key().size()));
                writeLimited(w, kv.value(), depth - 1);
            }
        }
        w.endObject();
    } else if (v.is<JsonArrayConst>()) {
        // A remote list or array: one value, whole at any depth.
        int d = depth > detail::kWholeDepth ? depth : detail::kWholeDepth;
        w.beginArray();
        for (JsonVariantConst e : v.as<JsonArrayConst>()) writeLimited(w, e, d);
        w.endArray();
    } else {
        w.variant(v);
    }
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

bool passes(const Subscription& sub, JsonString key) {
    std::string_view k(key.c_str(), key.size());
    if (!sub.keys.empty() && !inList(sub.keys, k)) return false;
    if (!sub.exclude.empty() && inList(sub.exclude, k)) return false;
    return true;
}

int depthOf(const Subscription& sub) { return sub.depth < 0 ? 255 : sub.depth; }

std::vector<std::string> listItems(std::string_view list) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string_view::npos) comma = list.size();
        std::string_view item = list.substr(pos, comma - pos);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty()) out.emplace_back(item);
        pos = comma + 1;
    }
    return out;
}

std::string joinSorted(std::vector<std::string> items) {
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
    std::string s;
    for (const std::string& i : items) {
        if (!s.empty()) s += ',';
        s += i;
    }
    return s;
}

// A subscription's snapshot, cut from the shared copy: keys and exclude at the
// top level, then depth, as for a get.
void writeSnapshot(JsonWriter& w, JsonVariantConst v, const Subscription& sub) {
    int depth = depthOf(sub);
    if (!v.is<JsonObjectConst>() || (sub.keys.empty() && sub.exclude.empty())) return writeLimited(w, v, depth);
    w.beginObject();
    if (depth > 0) {
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            if (!passes(sub, kv.key())) continue;
            w.key(std::string_view(kv.key().c_str(), kv.key().size()));
            writeLimited(w, kv.value(), depth - 1);
        }
    }
    w.endObject();
}

// Change notifications follow the local rule (subscription.cpp): values up
// to `depth` levels down, nothing from objects beyond it.
bool hasChanges(JsonVariantConst v, int depth, const Subscription* top) {
    if (depth <= 0) return false;
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
        if (top && !passes(*top, kv.key())) continue;
        if (!kv.value().is<JsonObjectConst>() || hasChanges(kv.value(), depth - 1, nullptr)) return true;
    }
    return false;
}

void writeChanges(JsonWriter& w, JsonVariantConst v, int depth, const Subscription* top) {
    w.beginObject();
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
        if (top && !passes(*top, kv.key())) continue;
        bool object = kv.value().is<JsonObjectConst>();
        if (object && !hasChanges(kv.value(), depth - 1, nullptr)) continue;
        w.key(std::string_view(kv.key().c_str(), kv.key().size()));
        if (object) {
            writeChanges(w, kv.value(), depth - 1, nullptr);
        } else {
            w.variant(kv.value());
        }
    }
    w.endObject();
}

// Sends one change notification to a forwarded subscriber.
void notifyChange(Subscription& sub, const std::function<void(JsonWriter&)>& body) {
    // Under the floor, even a small one: its allocations, unchecked under
    // 256 bytes, are what aborts on a heap taken by something else.
    if (!heapAbove(0)) {
        sub.subscriber->missed = true;
        return;
    }
    std::string msg;
    StringSink sink(msg, 0, true);
    JsonWriter m(sink);
    m.beginObject();
    m.key("op");
    m.string("change");
    m.key("path");
    m.string(sub.path);
    m.key("body");
    body(m);
    if (sub.ceiling < Access::Admin) {  // made with one: "sub@user"
        m.key("as");
        m.string(toString(sub.ceiling));
    }
    if (sub.subscriber->missed.exchange(false)) {
        m.key("overflow");
        m.boolean(true);
    }
    m.endObject();
    // There is nothing to resend from later: "overflow" tells the client to
    // re-read. So it is when the heap couldn't hold this one.
    if (!m.ok() || !sub.subscriber->notify(msg, Delivery::Reliable)) sub.subscriber->missed = true;
}

}  // namespace

// ---- upstream specs ------------------------------------------------------------

UpstreamSpec specOf(const Subscription& sub) {
    UpstreamSpec s;
    std::vector<std::string> keys = listItems(sub.keys);
    s.keys = joinSorted(keys);
    if (keys.empty()) s.exclude = joinSorted(listItems(sub.exclude));
    s.depth = sub.depth < 0 ? -1 : sub.depth;
    return s;
}

UpstreamSpec unionOf(const std::vector<UpstreamSpec>& specs) {
    UpstreamSpec u;
    if (specs.empty()) return u;
    bool unlimited = false, allKeyed = true, anyUnkeyed = false;
    int depth = 0;
    std::vector<std::string> keys, exclude;
    for (const UpstreamSpec& s : specs) {
        if (s.depth < 0) {
            unlimited = true;
        } else if (s.depth > depth) {
            depth = s.depth;
        }
        std::vector<std::string> k = listItems(s.keys);
        if (!k.empty()) {
            keys.insert(keys.end(), k.begin(), k.end());
            continue;
        }
        allKeyed = false;
        std::vector<std::string> e = listItems(s.exclude);
        if (!anyUnkeyed) {
            exclude = e;
            anyUnkeyed = true;
        } else {
            std::vector<std::string> both;
            for (const std::string& x : exclude) {
                if (std::find(e.begin(), e.end(), x) != e.end()) both.push_back(x);
            }
            exclude = both;
        }
    }
    u.depth = unlimited ? -1 : depth;
    if (allKeyed) {
        u.keys = joinSorted(keys);
    } else {
        std::vector<std::string> left;
        for (const std::string& x : exclude) {
            if (std::find(keys.begin(), keys.end(), x) == keys.end()) left.push_back(x);
        }
        u.exclude = joinSorted(left);
    }
    return u;
}

bool covers(const UpstreamSpec& have, const UpstreamSpec& need) { return unionOf({have, need}) == have; }

// ---- the mirror -------------------------------------------------------------------

void UpstreamCache::mirror(uint32_t intervalMs, uint32_t refreshMs, Access level) {
    mirror_ = true;
    mirrorLevel_ = level;
    mirrorIntervalMs_ = intervalMs;
    refreshMs_ = refreshMs;
}

bool UpstreamCache::hasCopy() const {
    MutexGuard guard(mutex_);
    return hasCopy_;
}

size_t UpstreamCache::count() const {
    MutexGuard guard(mutex_);
    return upstreams_.size();
}

void UpstreamCache::writeCopy(JsonWriter& w, int depth) const {
    MutexGuard guard(mutex_);
    if (hasCopy_) {
        writeLimited(w, copy_.as<JsonVariantConst>(), depth);
    } else {
        w.null();
    }
}

UpstreamCache::Upstream* UpstreamCache::find(std::string_view rest, Access level) {
    for (auto& u : upstreams_) {
        if (u->rest == rest && u->level == level) return u.get();
    }
    return nullptr;
}

void UpstreamCache::renew(RemoteContext node, uint32_t nowMs) {
    // Forwarded subscriptions: renewed every refreshMs in case the remote
    // node restarted, every 2 s after a failed renewal.
    struct Renewal {
        std::string rest;
        Access level;
        uint32_t interval;
    };
    std::vector<Renewal> renew;
    {
        MutexGuard guard(mutex_);
        for (auto& u : upstreams_) {
            if (u->subscribing || !u->ready) continue;
            uint32_t every = u->stale ? 2000 : refreshMs_;
            if (nowMs - u->lastSubscribeMs < every) continue;
            u->subscribing = true;
            u->lastSubscribeMs = nowMs;
            renew.push_back(Renewal{u->rest, u->level, u->interval});
        }
    }
    for (auto& r : renew) {
        if (!send(node, r.rest, r.level, r.interval, true)) {
            MutexGuard guard(mutex_);
            if (Upstream* u = find(r.rest, r.level)) u->subscribing = false;
        }
    }
}

void UpstreamCache::refreshMirror(RemoteContext node, const RemoteRoute& cur, uint32_t nowMs) {
    if (!mirror_ || subscribing_) return;
    if (subscribedOnce_) {
        // Renew: after refreshMs with a copy, every 2 s while there is none.
        uint32_t every = hasCopy() ? refreshMs_ : 2000;
        if (nowMs - lastSubscribeMs_ < every) return;
    }
    subscribedOnce_ = true;
    lastSubscribeMs_ = nowMs;
    subscribing_ = true;
    // Drop any previous subscription first, so renewals don't pile up.
    Query unsub;
    unsub.ceiling = mirrorLevel_;
    cur.endpoint->request(
        cur.peer, Op::Unsubscribe, node.remotePath(), std::string_view(), [](Status, JsonVariantConst) {}, unsub,
        node.timeoutMs(), &node.node());
    Query q;
    q.interval = mirrorIntervalMs_;
    q.remotes = false;  // never mirror the peer's own mirrors: no loops between gateways
    q.ceiling = mirrorLevel_;  // the copy is what a client at that level reads
    // The snapshot is parsed once, from the reply's text, straight into the
    // copy (the old one goes first), and only with room for it in the byte
    // budget: parsed as a reply, then copied, it was held three ways at
    // once, and a peer coming back after a restart took a gateway's free
    // heap 2 KB below its floor (DEVKIT-V3 mirroring DEVKIT-R1).
    bool sent = cur.endpoint->requestRaw(
        cur.peer, Op::Subscribe, node.remotePath(), JsonVariantConst(),
        [node](std::string_view text) {
            UpstreamCache& c = node.upstream();
            c.subscribing_ = false;
            // A document takes up to about three times its text in small
            // values, in PSRAM when the chip has it.
            Reservation room = memoryBudget().reserve(3 * text.size(), 0, false, true, Pool::Large);
            if (!room) return;  // retried in 2 s
            {
                MutexGuard guard(c.mutex_);
                c.copy_.clear();
                c.hasCopy_ = !parseJson(c.copy_, text, kWrittenNesting);
                if (!c.hasCopy_) c.copy_.clear();
            }
            node.changed();
        },
        [this](Status, JsonVariantConst) { subscribing_ = false; },  // retried in 2 s
        q, node.timeoutMs(), &node.node());
    if (!sent) subscribing_ = false;
}

void UpstreamCache::rerouted(RemoteContext node, const RemoteRoute& from, uint32_t nowMs) {
    std::vector<std::pair<std::string, Access>> paths;
    {
        MutexGuard guard(mutex_);
        for (auto& u : upstreams_) {
            if (!u->ready) continue;
            paths.emplace_back(node.remotePathOf(u->rest), u->level);
            u->stale = true;  // renewed below, on the new route
            u->lastSubscribeMs = nowMs - 2000;
        }
    }
    if (mirror_) {
        paths.emplace_back(node.remotePath(), mirrorLevel_);
        subscribedOnce_ = false;  // refreshMirror() subscribes again at once
    }
    // Back to a preferred route while the other one still works: drop what
    // the device sends that way. (A lost route's subscriptions end on their
    // own: the device fails to deliver to it.)
    if (!from.endpoint || !from.online || !node.node().hasRoute(from.endpoint, from.peer)) return;
    for (const auto& p : paths) {
        Query q;
        q.ceiling = p.second;
        from.endpoint->request(
            from.peer, Op::Unsubscribe, p.first, std::string_view(), [](Status, JsonVariantConst) {}, q,
            node.timeoutMs(), &node.node());
    }
}

bool UpstreamCache::notified(RemoteContext node, std::string_view rest, Access level, std::string_view op,
                             JsonObjectConst envelope) {
    bool forMirror = mirror_ && level == mirrorLevel_;
    if (op == "change") {
        if (!forMirror || !rest.empty()) return relayChange(node, rest, level, envelope["body"]);
        {
            MutexGuard guard(mutex_);
            // Merged, the change takes about twice its text more in the
            // copy: only within the budget (docs/DESIGN.md section 15). Else
            // the copy goes, and is taken again whole within 2 s.
            Reservation room = memoryBudget().reserve(2 * measureJson(envelope["body"]), 0, false, true, Pool::Large);
            if (!room) {
                copy_.clear();
                hasCopy_ = false;
            } else if (hasCopy_) {
                mergeJson(copy_.as<JsonVariant>(), envelope["body"]);
            } else {
                copy_.set(envelope["body"]);
                hasCopy_ = true;
            }
        }
        node.changed();
        return true;
    }
    if (op == "event") {
        if (!forMirror && count() == 0) return false;
        std::string local;
        if (!node.findLocalPath(local)) return true;
        local.append(rest.data(), rest.size());
        JsonVariantConst body = envelope["body"];
        // It came at `level`, once for every subscription of this node's at
        // that level it concerns: for the local subscribers at that level
        // below this node, and, from the mirror, for those of the copy.
        const Node* self = &node.node();
        node.api()->emitEvent(
            local, [body](JsonWriter& w) { w.variant(body); },
            [self, level, forMirror](const Subscription& sub) {
                if (sub.forwarded) return sub.node == self && sub.access == level;
                return forMirror && sub.access >= level;
            });
        return true;
    }
    return false;
}

// ---- shared subscriptions ----------------------------------------------------------

void UpstreamCache::subscribe(RemoteContext node, Api& api, const Request& req, std::string_view rest,
                              std::string_view base, Reply& reply) {
    std::string_view basePath = base.empty() ? std::string_view("/") : base;
    if (!req.subscriber) return writeError(reply, Status::NotAllowed, Reason::CannotWait, basePath, "this transport can't deliver notifications");
    if (!req.body.isNull()) {
        return writeError(reply, Status::BadRequest, basePath, "subscriptions take keys, exclude and depth, not shapes");
    }
    if (req.query.view != View::Value) return writeError(reply, Status::BadRequest, basePath, "subscriptions are on values");
    RemoteRoute route;
    if (!node.route(route)) return writeError(reply, Status::Internal, basePath, "no endpoint");
    Status allowed = api.subscriptionAllowed(req.subscriber);
    if (allowed != Status::Ok) return writeError(reply, allowed, Reason::SubscriptionLimit, basePath, "too many subscriptions");

    node.adopt(api, base.substr(0, base.size() - rest.size()));

    Subscription sub;
    sub.subscriber = req.subscriber;
    sub.node = &node.node();
    sub.path.assign(base.data(), base.size());
    sub.keys.assign(req.query.keys.data(), req.query.keys.size());
    sub.exclude.assign(req.query.exclude.data(), req.query.exclude.size());
    sub.depth = req.query.depth;
    sub.interval = req.query.interval;
    sub.changes = req.query.changes;
    sub.events = req.query.events;
    sub.eventHint.assign(req.query.eventHint.data(), req.query.eventHint.size());
    sub.remotes = req.query.remotes;
    sub.forwarded = true;
    sub.snapshot = req.query.snapshot;
    // Shared only with subscribers at the same level, which the upstream
    // subscription asks the remote for (docs/DESIGN.md section 13.1).
    sub.access = req.client.level();
    sub.ceiling = req.query.ceiling;
    Access level = sub.access;

    bool start = false, renew = false;
    uint32_t interval = 0;
    // What the upstream subscription must hold once this one joins.
    UpstreamSpec want = wanted(node, sub.path, level, &sub);
    {
        MutexGuard guard(mutex_);
        Upstream* u = find(rest, level);
        if (u && u->ready && covers(u->copySpec, specOf(sub))) {
            // Shared: the snapshot comes from the copy, right away.
            JsonWriter w(reply.begin(Status::Ok));
            if (sub.snapshot) {
                writeSnapshot(w, u->copy.as<JsonVariantConst>(), sub);
            } else {
                w.null();
            }
            detail::finishBody(reply, w, basePath);
            api.addSubscription(std::move(sub));
            return;
        }
        if (!node.online()) {
            std::string localBase(base.substr(0, base.size() - rest.size()));
            return writeError(reply, Status::Timeout, Reason::PeerOffline, localBase.empty() ? std::string_view("/") : localBase,
                              "peer offline");
        }
        // A new or wider copy of the remote's state, and the snapshots
        // rendered from it: only with room to spare above the heap floor,
        // in internal RAM and in PSRAM, where the copy goes when the chip
        // has it.
        if (!heapAbove(heapFloor()) || !heapAbove(heapFloor(Pool::Large), Pool::Large)) {
            return writeError(reply, Status::Busy, Reason::LowMemory, basePath, "not enough memory for another remote subscription");
        }
        if (!api.pendingBegin()) return writeError(reply, Status::Busy, Reason::PendingLimit, basePath, "too many pending calls");
        Reply* detached = reply.detach();
        if (!detached) {
            api.pendingDone();
            return writeError(reply, Status::NotAllowed, Reason::CannotWait, basePath, "this transport can't wait for remote nodes");
        }
        sub.waiting = detached;
        if (!u) {
            upstreams_.emplace_back(new Upstream());
            u = upstreams_.back().get();
            u->rest.assign(rest.data(), rest.size());
            u->level = level;
            // An events-only subscriber doesn't pace the others' changes.
            u->interval = sub.changes ? req.query.interval : 0;
        }
        // A copy that doesn't hold what this one needs: subscribed again,
        // wider; this one waits for that snapshot.
        u->spec = want;
        interval = u->interval;
        if (!u->subscribing) {
            u->subscribing = true;
            u->lastSubscribeMs = route.endpoint->now();
            start = true;
            renew = u->ready;
        }
    }
    std::string r(rest);
    api.addSubscription(std::move(sub));  // waiting for the upstream snapshot
    if (start && !send(node, r, level, interval, renew)) answered(node, r, level, Status::Busy, JsonVariantConst());
}

UpstreamSpec UpstreamCache::wanted(RemoteContext node, const std::string& local, Access level,
                                   const Subscription* extra) {
    std::vector<UpstreamSpec> specs;
    if (extra) specs.push_back(specOf(*extra));
    if (Api* api = node.api()) {
        api->forwardedSubscriptions(&node.node(), [&](Subscription& sub) {
            if (sub.path == local && sub.access == level) specs.push_back(specOf(sub));
            return true;
        });
    }
    return unionOf(specs);
}

void UpstreamCache::narrowed(RemoteContext node, const std::string& rest, std::string_view localPath, Access level) {
    UpstreamSpec want = wanted(node, std::string(localPath), level, nullptr);
    uint32_t interval;
    {
        MutexGuard guard(mutex_);
        Upstream* u = find(rest, level);
        if (!u || u->spec == want) return;
        u->spec = want;
        // In flight: its reply sees the new spec and asks again.
        if (u->subscribing || !u->ready) return;
        u->subscribing = true;
        interval = u->interval;
    }
    if (!send(node, rest, level, interval, true)) {
        MutexGuard guard(mutex_);
        if (Upstream* u = find(rest, level)) u->subscribing = false;
    }
}

bool UpstreamCache::sent(std::string_view rest, Access level, UpstreamSpec& out) const {
    MutexGuard guard(mutex_);
    for (const auto& u : upstreams_) {
        if (u->rest != rest || u->level != level) continue;
        out = u->sent;
        return true;
    }
    return false;
}

bool UpstreamCache::send(RemoteContext node, const std::string& rest, Access level, uint32_t interval, bool renew) {
    RemoteRoute r;
    if (!node.route(r)) return false;
    std::string path = node.remotePathOf(rest);
    if (renew) {
        Query unsub;
        unsub.ceiling = level;
        r.endpoint->request(
            r.peer, Op::Unsubscribe, path, std::string_view(), [](Status, JsonVariantConst) {}, unsub,
            node.timeoutMs(), &node.node());
    }
    UpstreamSpec spec;
    {
        MutexGuard guard(mutex_);
        if (Upstream* u = find(rest, level)) {
            u->sent = u->spec;
            spec = u->spec;
        }
    }
    // Only what the local subscribers need (keys, depth): a whole tree
    // would cost the remote, the link and this node's heap.
    Query q;
    q.interval = interval;
    q.keys = spec.keys;
    q.exclude = spec.exclude;
    q.depth = spec.depth;
    q.ceiling = level;  // the remote filters as for these subscribers
    return r.endpoint->request(
        r.peer, Op::Subscribe, path, std::string_view(),
        [node, rest, level](Status s, JsonVariantConst body) { node.upstream().answered(node, rest, level, s, body); },
        q, node.timeoutMs(), &node.node());
}

void UpstreamCache::answered(RemoteContext node, const std::string& rest, Access level, Status s,
                             JsonVariantConst body) {
    Api* api = node.api();
    if (!api) return;
    bool again = false;
    uint32_t interval = 0;
    {
        // Lock order: the API, then this cache.
        MutexGuard apiGuard(api->mutex());
        MutexGuard guard(mutex_);
        Upstream* u = find(rest, level);
        if (!u) {
            // Released while subscribing: drop the subscription it made.
            RemoteRoute r;
            if (s == Status::Ok && node.route(r)) {
                Query unsub;
                unsub.ceiling = level;
                r.endpoint->request(
                    r.peer, Op::Unsubscribe, node.remotePathOf(rest), std::string_view(),
                    [](Status, JsonVariantConst) {}, unsub, node.timeoutMs(), &node.node());
            }
            return;
        }
        u->subscribing = false;
        // A copy of the remote's state takes about twice its text: only
        // above the heap floor; else as if the remote were busy (kept stale,
        // tried again soon).
        JsonDocument noRoom;
        if (s == Status::Ok && !heapAbove(2 * measureJson(body), Pool::Large)) {
            noRoom["error"] = "busy";
            noRoom["reason"] = "low_memory";
            noRoom["path"] = node.remotePathOf(rest);
            noRoom["message"] = "not enough memory for the remote's state";
            s = Status::Busy;
            body = noRoom.as<JsonVariantConst>();
        }
        bool wasReady = u->ready;
        bool changedCopy = false;
        bool reshaped = false;  // the copy now holds more or less, not newer state
        if (s == Status::Ok) {
            reshaped = wasReady && u->copySpec != u->sent;
            // Compared in place: two serialized copies of a large state could
            // exhaust a fragmented heap.
            changedCopy = !wasReady || (!reshaped && u->copy.as<JsonVariantConst>() != body);
            u->copy.set(body);
            u->copySpec = u->sent;
            u->ready = true;
            u->stale = false;
        } else if (wasReady) {
            u->stale = true;  // keep the copy and the subscribers; retry soon
        }

        std::string localPath = node.localPath() + rest;
        std::string remoteBase = node.remotePath() == "/" ? std::string() : node.remotePath();
        std::string localBase = node.localPath();
        bool anyLeft = false, stillWaiting = false;
        JsonVariantConst copy = u->copy.as<JsonVariantConst>();
        api->forwardedSubscriptions(&node.node(), [&](Subscription& sub) {
            if (sub.path != localPath || sub.access != level) return true;
            if (sub.waiting) {
                if (s == Status::Ok && !covers(u->copySpec, specOf(sub))) {
                    // Joined while a narrower request was in flight: next one.
                    stillWaiting = true;
                    anyLeft = true;
                    return true;
                }
                if (s == Status::Ok) {
                    JsonWriter w(sub.waiting->begin(Status::Ok));
                    if (sub.snapshot) {
                        writeSnapshot(w, copy, sub);
                    } else {
                        w.null();
                    }
                    detail::finishBody(*sub.waiting, w, sub.path);
                } else {
                    writeMapped(*sub.waiting, s, body, localBase, remoteBase);
                }
                sub.waiting->release();
                sub.waiting = nullptr;
                api->pendingDone();
                if (s != Status::Ok) return false;
            } else if (s == Status::Ok && wasReady && changedCopy && sub.changes) {
                // Renewed after the remote changed without telling us (it
                // restarted, or notifications were lost): send the whole state.
                notifyChange(sub, [&](JsonWriter& w) { writeSnapshot(w, copy, sub); });
            }
            anyLeft = true;
            return true;
        });
        if (!anyLeft && !u->ready) {
            for (size_t i = 0; i < upstreams_.size(); i++) {
                if (upstreams_[i].get() == u) {
                    upstreams_.erase(upstreams_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            return;
        }
        // The subscribers' needs changed while it was in flight.
        if (s == Status::Ok && (stillWaiting || u->spec != u->sent)) {
            u->subscribing = true;
            interval = u->interval;
            again = true;
        }
    }
    if (again && !send(node, rest, level, interval, true)) {
        MutexGuard guard(mutex_);
        if (Upstream* u = find(rest, level)) {
            u->subscribing = false;
            u->stale = true;  // renew() tries again
        }
    }
}

bool UpstreamCache::relayChange(RemoteContext node, std::string_view rest, Access level, JsonVariantConst patch) {
    Api* api = node.api();
    if (!api) return false;
    MutexGuard apiGuard(api->mutex());
    MutexGuard guard(mutex_);
    Upstream* u = find(rest, level);
    if (!u || !u->ready) return false;
    // Only within the budget (docs/DESIGN.md section 15): else the change
    // isn't relayed, and the subscription is renewed soon, its subscribers
    // getting the whole state if it differs.
    Reservation room = memoryBudget().reserve(2 * measureJson(patch), 0, false, true, Pool::Large);
    if (!room) {
        u->stale = true;  // renewed within 2 s
        return true;
    }
    mergeJson(u->copy.as<JsonVariant>(), patch);
    std::string localPath = node.localPath() + std::string(rest);
    api->forwardedSubscriptions(&node.node(), [&](Subscription& sub) {
        if (sub.path != localPath || sub.access != level || sub.waiting || !sub.changes) return true;
        if (!patch.is<JsonObjectConst>()) {
            notifyChange(sub, [&](JsonWriter& w) { w.variant(patch); });
        } else if (hasChanges(patch, depthOf(sub), &sub)) {
            notifyChange(sub, [&](JsonWriter& w) { writeChanges(w, patch, depthOf(sub), &sub); });
        }
        return true;
    });
    return true;
}

void UpstreamCache::released(RemoteContext node, const std::string& rest, Access level) {
    bool found = false;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < upstreams_.size(); i++) {
            if (upstreams_[i]->rest != rest || upstreams_[i]->level != level) continue;
            found = upstreams_[i]->ready;  // still subscribing: answered() unsubscribes
            upstreams_.erase(upstreams_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    RemoteRoute r;
    if (found && node.route(r)) {
        Query unsub;
        unsub.ceiling = level;
        r.endpoint->request(
            r.peer, Op::Unsubscribe, node.remotePathOf(rest), std::string_view(), [](Status, JsonVariantConst) {},
            unsub, node.timeoutMs(), &node.node());
    }
}

}  // namespace detail
}  // namespace tesser
