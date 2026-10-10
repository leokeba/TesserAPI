#pragma once

// Internal helpers shared by subscriptions and persistence.

#include "tesser/json_writer.h"
#include "tesser/node.h"

namespace tesser {
namespace detail {

// Writes a sparse object: an object's key is written only once something
// changed below it, so unchanged branches cost nothing.
class LazyPatch {
public:
    static constexpr int kMax = 32;
    explicit LazyPatch(JsonWriter& w) : w_(w) {}

    bool enter(const char* name) {
        if (depth_ >= kMax) return false;
        stack_[depth_++] = name;
        return true;
    }
    void leave() {
        depth_--;
        if (opened_ > depth_) {
            w_.endObject();
            opened_ = depth_;
        }
    }
    // Opens the enclosing objects and writes the key of a changed value.
    void key(const char* name) {
        if (!rootOpen_) {
            w_.beginObject();
            rootOpen_ = true;
        }
        for (; opened_ < depth_; opened_++) {
            w_.key(stack_[opened_]);
            w_.beginObject();
        }
        w_.key(name);
    }
    // Returns true if anything was written.
    bool finish() {
        while (opened_ > 0) {
            w_.endObject();
            opened_--;
        }
        if (rootOpen_) w_.endObject();
        return rootOpen_;
    }

private:
    JsonWriter& w_;
    const char* stack_[kMax];
    int depth_ = 0;
    int opened_ = 0;
    bool rootOpen_ = false;
};

// What a value render includes.
struct RenderOptions {
    bool remotes = true;            // mirrored remote nodes' copies
    Access access = Access::Admin;  // children needing more to be read are left out
    bool secrets = false;           // secret values as themselves (persistence), else null
};

// Value view of any node, without filters (defined in api.cpp).
void renderValue(JsonWriter& w, const Node& n, int depth, const RenderOptions& options = RenderOptions());

// Whether a node appears in the value view (actions, events and files don't).
inline bool hasValue(const Node& n) {
    return n.type() != NodeType::Action && n.type() != NodeType::Event && n.type() != NodeType::File;
}

// A node subscriptions and persistence treat as one value.
inline bool isLeaf(const Node& n) {
    return n.type() == NodeType::Value || n.type() == NodeType::Custom || n.type() == NodeType::List ||
           n.type() == NodeType::Array ||
           n.type() == NodeType::Remote;
}

inline void writeLeaf(JsonWriter& w, const Node& n, const RenderOptions& options = RenderOptions()) {
    renderValue(w, n, 16, options);
}

}  // namespace detail
}  // namespace tesser
