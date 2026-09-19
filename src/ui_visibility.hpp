#pragma once

#include <string_view>

namespace cleanfeed::ui_visibility {
    inline bool isXdbotID(std::string_view id) {
        return id.starts_with("zilko.xdbot/");
    }

    // Visit only visible UI subtrees. Ownership is tested before leaf pruning:
    // an owned label is a batch node, but its glyphs need not be inspected.
    template <class Node, class Owned, class Prune, class Children, class Defer>
    void collect(Node* node, Owned const& owned, Prune const& prune,
                 Children const& children, Defer const& defer) {
        if (!node || !node->isVisible()) return;
        if (owned(node)) {
            defer(node);
            return;
        }
        if (prune(node)) return;
        for (auto* child : children(node)) collect(child, owned, prune, children, defer);
    }

    template <class Node>
    bool visibleUnder(Node* node, Node* root) {
        for (auto* current = node; current; current = current->getParent()) {
            if (!current->isVisible()) return false;
            if (current == root) return true;
        }
        return false;
    }
}
