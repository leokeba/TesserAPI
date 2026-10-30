#include "tesser/detail/api_handler.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/envelope.h"
#include "tesser/file.h"
#include "tesser/remote.h"

// A request's walk down the path, and how it fails (detail::Handler).

namespace tesser {
namespace detail {

bool parseIndex(std::string_view s, size_t& out) {
    if (s.empty() || s.size() > 9 || (s.size() > 1 && s[0] == '0')) return false;
    size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}

void Handler::run() {
    Node* target = resolve();
    if (!target) return;
    if (star_) return fanOut(*target);
    if (req_.op != Op::Unsubscribe) {
        Access need = req_.op == Op::Set ? writeNeed_ : readNeed_;
        if (level_ < need || !allowed(*target)) return replyError(Status::Unauthorized, Reason::NotAuthorized, "not authorized", base_);
    }
    if (target->type() == NodeType::Remote) {
        auto& remote = static_cast<RemoteNode&>(*target);
        bool itself = remoteRest_.empty();
        if (req_.op == Op::Get || req_.op == Op::Set) return forward(remote);
        // A mirrored remote node itself is subscribed to locally, like a
        // value, by clients the copy is for; anything else is forwarded.
        // Unsubscribing is local.
        bool local = itself && remote.mirrored() && level_ >= remote.mirrorLevel();
        if (req_.op == Op::Subscribe && !local) {
            return remote.subscribe(api_, req_, remoteRest_, base_, reply_);
        }
    }
    switch (req_.op) {
        case Op::Get: get(*target); break;
        case Op::Set: set(*target); break;
        case Op::Subscribe: subscribe(*target); break;
        case Op::Unsubscribe: unsubscribe(); break;
    }
}

// The file node a transfer targets (FileTransfer), or null with the error
// written; `notFile` (no error) when the path resolves to something else.
// The elements on the path move to `elements`, which the node lives in.
// Below a remote node, `remote` (if given) is set instead, with nothing
// written: the transfer is relayed.
FileNode* Handler::file(bool& notFile, std::vector<std::unique_ptr<Node>>& elements, Api::RemoteTarget* remote) {
    Node* target = resolve();
    elements = std::move(temps_);
    if (!target) return nullptr;
    if (star_) {
        replyError(Status::BadRequest, "'*' doesn't apply to files", base_);
        return nullptr;
    }
    bool relayed = remote && target->type() == NodeType::Remote && !remoteRest_.empty();
    if (target->type() != NodeType::File && !relayed) {
        notFile = true;
        return nullptr;
    }
    Access need = req_.op == Op::Set ? writeNeed_ : readNeed_;
    if (level_ < need || !allowed(*target)) {
        replyError(Status::Unauthorized, Reason::NotAuthorized, "not authorized", base_);
        return nullptr;
    }
    if (relayed) {
        remote->node = static_cast<RemoteNode*>(target);
        remote->rest.assign(remoteRest_.data(), remoteRest_.size());
        remote->base.assign(base_.data(), base_.size());
        remote->level = level_;
        return nullptr;
    }
    return static_cast<FileNode*>(target);
}

std::string Handler::trailPath() const {
    std::string p(base_.data(), base_.size());
    for (int i = 0; i < trailLen_; i++) {
        p += '/';
        if (indexMask_ & (uint32_t(1) << i)) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", unsigned(indexes_[i]));
            p += buf;
        } else {
            p.append(trail_[i].data(), trail_[i].size());
        }
    }
    if (p.empty()) p = "/";
    return p;
}

bool Handler::push(std::string_view name) {
    if (trailLen_ >= kMaxTrail) return false;
    indexMask_ &= ~(uint32_t(1) << trailLen_);
    trail_[trailLen_++] = name;
    return true;
}

// A list index in the trail: stored as a number, formatted on error only.
bool Handler::pushIndex(size_t i) {
    if (trailLen_ >= kMaxTrail) return false;
    indexMask_ |= uint32_t(1) << trailLen_;
    indexes_[trailLen_++] = static_cast<uint16_t>(i);
    return true;
}

// Records the first error; the reply is written once the walk unwinds.
bool Handler::error(Status s, const char* message, Reason reason) {
    if (!failed_) {
        failed_ = true;
        errStatus_ = s;
        errReason_ = reason;
        errMessage_ = message;
        errPath_ = trailPath();
    }
    return false;
}

Node* Handler::resolve() {
    std::string_view path = req_.path;
    require(api_);
    if (path.empty() || path == "/") return &api_;
    if (path.front() != '/') {
        replyError(Status::BadRequest, "path must start with '/'", path);
        return nullptr;
    }
    Node* node = &api_;
    size_t pos = 1;
    std::string_view p = base_;
    while (pos < p.size()) {
        if (node->type() == NodeType::Remote) {
            remoteRest_ = p.substr(pos - 1);  // forwarded as is
            break;
        }
        size_t slash = p.find('/', pos);
        if (slash == std::string_view::npos) slash = p.size();
        std::string_view seg = p.substr(pos, slash - pos);
        if (seg.empty()) {
            replyError(Status::BadRequest, "empty path segment", p);
            return nullptr;
        }
        if (seg == "*") {
            if (req_.op != Op::Set) {
                replyError(Status::BadRequest, "only set takes '*'", p);
                return nullptr;
            }
            star_ = pos;  // fanOut() expands it
            return node;
        }
        Node* next = nullptr;
        if (node->type() == NodeType::Object) {
            next = static_cast<Object*>(node)->child(seg);
        } else if (node->type() == NodeType::List) {
            auto* list = static_cast<ListNode*>(node);
            size_t index = list->size();
            if (list->keyField()) {
                index = list->find(seg);
            } else if (!parseIndex(seg, index)) {
                index = list->size();
            }
            if (index < list->size()) {
                std::unique_ptr<Object> element = list->element(index);
                if (list->keyField()) keyRefs_.push_back({element->child(list->keyField()), list, index});
                temps_.push_back(std::move(element));
                containers_.push_back(list);
                next = temps_.back().get();
            }
        } else if (node->type() == NodeType::Array) {
            auto* array = static_cast<ArrayNode*>(node);
            size_t index;
            if (parseIndex(seg, index) && index < array->size()) {
                temps_.push_back(array->element(index));
                containers_.push_back(array);
                next = temps_.back().get();
            }
        }
        if (!next) {
            replyError(Status::NotFound, "no such node", p.substr(0, slash));
            return nullptr;
        }
        require(*next);
        node = next;
        pos = slash + 1;
    }
    return node;
}
void Handler::forward(RemoteNode& remote) { remote.forward(api_, req_, remoteRest_, base_, reply_); }

}  // namespace detail
}  // namespace tesser
