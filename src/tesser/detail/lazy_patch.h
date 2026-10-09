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

inline void writeLeaf(JsonWriter& w, const Node& n) {
    if (n.type() == NodeType::Value) {
        static_cast<const ValueNode&>(n).write(w);
    } else {
        static_cast<const CustomNode&>(n).write(w);
    }
}

}  // namespace detail
}  // namespace tesser
