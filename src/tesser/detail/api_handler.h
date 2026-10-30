#pragma once

// One request in flight (docs/DESIGN.md section 16): what Api::handle() runs,
// under the API lock. Its code is in api_resolve.cpp (the walk down the path,
// errors), api_render.cpp (reading: values, schemas, subscriptions) and
// api_patch.cpp (writing: validating, then applying).

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tesser/api.h"
#include "tesser/detail/fan_out.h"
#include "tesser/detail/lazy_patch.h"

namespace tesser {
namespace detail {

// Calls f(item) for each non-empty, space-trimmed item of a comma-separated list.
template <class F>
bool forEachItem(std::string_view list, F f) {
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string_view::npos) comma = list.size();
        std::string_view item = list.substr(pos, comma - pos);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty() && !f(item)) return false;
        pos = comma + 1;
    }
    return true;
}

inline bool listContains(std::string_view list, std::string_view name) {
    return !forEachItem(list, [&](std::string_view item) { return item != name; });
}

bool parseIndex(std::string_view s, size_t& out);

inline Access higher(Access a, Access b) { return a > b ? a : b; }

struct Filter {
    std::string_view keys;
    std::string_view exclude;
    JsonObjectConst shape;
    bool hasShape = false;
};

// Built as one unit (ESP-IDF, docs/DESIGN.md section 16), it has internal
// linkage, as when it all was in one file: -Os inlines what has one caller,
// which saves flash and keeps the recursive walkers' frames as measured.
#if defined(TESSER_UNITY_BUILD)
namespace {
#endif

// One request in flight. Holds the error state and the path trail used to
// name the failing node in error replies.
class Handler {
public:
    static constexpr int kMaxTrail = 32;

    Handler(Api& api, const Request& req, Reply& reply) : api_(api), req_(req), reply_(reply) {
        level_ = req.client.level();
        render_.remotes = req.query.remotes;
        render_.access = level_;
        base_ = req.path;
        if (base_.size() > 1 && base_.back() == '/') base_.remove_suffix(1);
        if (base_ == "/") base_ = std::string_view();
        int maxDepth = api.config().maxDepth;
        depth_ = (req.query.depth < 0 || req.query.depth > maxDepth) ? maxDepth : req.query.depth;
    }

    void run();

    // The file node a transfer targets (FileTransfer), or null with the error
    // written; `notFile` (no error) when the path resolves to something else.
    // The elements on the path move to `elements`, which the node lives in.
    // Below a remote node, `remote` (if given) is set instead, with nothing
    // written: the transfer is relayed.
    FileNode* file(bool& notFile, std::vector<std::unique_ptr<Node>>& elements, Api::RemoteTarget* remote);

private:
    // A write below a list element marks the list itself changed: element
    // nodes are temporary, the list is what subscribers and persistence see.
    void markLists() {
        for (Node* l : containers_) l->changed();
    }

    bool allowed(const Node& n) const {
        const Authorizer& a = api_.authorizer();
        return !a || a(req_.client, req_.op, n);
    }

    // Whether a child of an already readable node may be read.
    bool readable(const Node& n) const { return n.readAccess() <= level_; }

    // ---- errors -------------------------------------------------------

    std::string trailPath() const;

    bool push(std::string_view name);
    void pop() { trailLen_--; }

    // A list index in the trail: stored as a number, formatted on error only.
    bool pushIndex(size_t i);

    // Records the first error; the reply is written once the walk unwinds.
    bool error(Status s, const char* message, Reason reason = Reason::None);

    void replyError() { writeError(reply_, errStatus_, errReason_, errPath_, errMessage_, partial_); }

    void replyError(Status s, const char* message, std::string_view path) {
        replyError(s, Reason::None, message, path);
    }
    void replyError(Status s, Reason reason, const char* message, std::string_view path) {
        writeError(reply_, s, reason, path.empty() ? std::string_view("/") : path, message);
    }

    // ---- resolution ---------------------------------------------------

    // The levels needed to read and write `n`, given its parent's.
    void require(const Node& n) {
        readNeed_ = higher(readNeed_, n.readAccess());
        writeNeed_ = higher(writeNeed_, n.writeAccess());
    }

    Node* resolve();

    // ---- get ----------------------------------------------------------

    // The schema this client would read, whole, without the state of remote
    // nodes (online, advertised hash), which changes with peers and reaches
    // subscribers as changes. Otherwise two gateways mounting each other
    // would re-advertise each other's hashes forever. 8 hex digits.
    void hashHex(Node& target, char (&hex)[9]);

    // view=info (docs/DESIGN.md section 10.1.2): what a client reads before
    // relying on newer options: the protocol, the features, and what
    // view=hash gives.
    bool info(Node& target);

    // Replies with the target's representation; true if that was a success.
    bool get(Node& target);

    bool validateShape(const Object& obj, JsonObjectConst shape);

    // ---- rendering ----------------------------------------------------

    void renderAny(JsonWriter& w, const Node& n, int depth, const Filter& f, View view);

    void renderChildren(JsonWriter& w, const Object& o, int depth, const Filter& f, View view);

    // Between an object's children: a streamed reply's pending pieces go out
    // without the API lock (docs/DESIGN.md section 9), so a slow line holds
    // up nobody else. Gets only (a subscription's snapshot must stay one
    // with its start), and never below a list element, whose nodes the
    // application's data backs. What a reply renders after the lock came
    // back may be newer than what it rendered before.
    void yieldPoint();

    // Out of line, like schemaAttributes, so renders without a shape don't
    // carry its frame.
    __attribute__((noinline)) void renderShaped(JsonWriter& w, const Object& o, int depth, const Filter& f, View view);

    void renderSchema(JsonWriter& w, const Node& n, int depth, const Filter& f);

    // Everything but the children, out of line: the recursion through
    // renderSchema and renderChildren then keeps small frames (with -Og, the
    // ESP-IDF default, this one alone takes over 500 bytes).
    __attribute__((noinline)) void schemaAttributes(JsonWriter& w, const Node& n);

    static void renderMeta(JsonWriter& w, const NodeMeta& m);

    // ---- remote nodes -------------------------------------------------

    // Virtual, so the datagram and forwarding code is only linked into
    // applications that create remote nodes.
    void forward(RemoteNode& remote);

    // ---- '*' ----------------------------------------------------------

    // A set whose path has a '*' segment, at star_: one set per child of
    // `parent`, each run like a request of its own (docs/DESIGN.md section
    // 6.5). Children the client can't read aren't matched.
    __attribute__((noinline)) void fanOut(Node& parent);

    // ---- subscriptions ------------------------------------------------

    void subscribe(Node& target);

    void unsubscribe();

    // ---- set ----------------------------------------------------------

    void set(Node& target);

    void setObject(Object& obj, JsonVariantConst body);

    // A keyed list replaced by an array: every item names its element by a
    // valid key, once; it patches that element, or a default one.
    bool validateKeyed(ListNode& list, JsonArrayConst items, Access need);

    // A write to the key of a keyed list's element (renaming it, reached
    // through /list/<key>) must give a valid key no other element has.
    bool keyWriteOk(const Node& n, JsonVariantConst v);

    // An action's object argument: a default one, validated and filled from
    // `arg` like a patch, so errors name the field (/add/az). Null (with the
    // error recorded) if `arg` doesn't fit.
    std::unique_ptr<Object> fillParams(const ActionNode& a, JsonVariantConst arg);

    // `need`: the level writing `n` takes, its ancestors' included.
    bool validatePatch(Node& n, JsonVariantConst v, Access need);

    // The cases that don't recurse, and lists, out of line: walking a deep
    // object then keeps small frames (with -Og, the ESP-IDF default).
    __attribute__((noinline)) bool validateLeaf(Node& n, JsonVariantConst v);

    __attribute__((noinline)) bool validateList(ListNode& list, JsonVariantConst v, Access need);

    // actions == false: apply values and custom nodes; true: run actions.
    bool applyPatch(Node& n, JsonVariantConst v, bool actions);

    __attribute__((noinline)) bool applyLeaf(Node& n, JsonVariantConst v, bool actions);

    __attribute__((noinline)) bool applyList(ListNode& list, JsonVariantConst v, bool actions);

    Api& api_;
    const Request& req_;
    Reply& reply_;
    std::string_view base_;  // request path without trailing slash; empty for the root
    int depth_;
    Access level_;                       // the client's
    Access readNeed_ = Access::Public;   // what the target needs, its ancestors' included
    Access writeNeed_ = Access::Public;
    detail::RenderOptions render_;

    std::string_view trail_[kMaxTrail];
    int trailLen_ = 0;
    bool failed_ = false;
    Status errStatus_ = Status::Internal;
    const char* errMessage_ = nullptr;
    Reason errReason_ = Reason::None;
    std::string errPath_;
    int applied_ = 0;
    bool partial_ = false;
    bool filling_ = false;  // validating or filling an action's object argument
    bool hashing_ = false;  // rendering a schema for view=hash
    int inList_ = 0;        // rendering a list's prototype: no yieldPoint()
    std::vector<std::unique_ptr<Node>> temps_;  // list and array elements on the request path
    std::vector<Node*> containers_;             // lists and arrays the path went through
    struct KeyRef {
        const Node* node;  // the key value of an element on the path
        ListNode* list;
        size_t index;
    };
    std::vector<KeyRef> keyRefs_;
    uint16_t indexes_[kMaxTrail];                 // list indexes in the trail...
    uint32_t indexMask_ = 0;                      // ...at the levels whose bit is set
    std::string_view remoteRest_;                 // path below a remote node, forwarded as is
    size_t star_ = 0;                             // position of a '*' segment in base_ (set only)
};

#if defined(TESSER_UNITY_BUILD)
}  // namespace
#endif

}  // namespace detail
}  // namespace tesser
