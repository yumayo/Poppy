#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace poppy {
using TreeItem = uintptr_t;
// Window-independent hierarchy. IDs are never reused, including after clear(),
// so an asynchronous completion cannot address a replacement item by mistake.
class TreeModel {
public:
    struct Node {
        TreeItem parent = 0;
        std::wstring text;
        uintptr_t data = 0;
        uint32_t color = 0;
        bool expandable = false, expanded = false;
        std::vector<TreeItem> children;
        TreeItem next = 0;
    };
    struct Row { TreeItem item; size_t depth; };
    TreeItem insert(TreeItem parent, std::wstring text, bool expandable, uintptr_t data, uint32_t color);
    const Node* get(TreeItem item) const;
    void label(TreeItem item, std::wstring text, uint32_t color);
    bool expand(TreeItem item, bool expanded);
    bool select(TreeItem item);
    TreeItem selection() const { return selection_; }
    TreeItem child(TreeItem parent = 0) const;
    TreeItem next(TreeItem item) const;
    TreeItem parent(TreeItem item) const;
    size_t size() const { return nodes_.size(); }
    const std::vector<Row>& visible() const;
    std::vector<std::pair<TreeItem, uintptr_t>> erase(TreeItem item);
    std::vector<std::pair<TreeItem, uintptr_t>> eraseChildren(TreeItem parent);
    std::vector<std::pair<TreeItem, uintptr_t>> clear();
    void sort(TreeItem parent, const std::function<bool(uintptr_t, uintptr_t)>& compare);
private:
    bool descendant(TreeItem item, TreeItem ancestor) const;
    void eraseSubtree(TreeItem item, std::vector<std::pair<TreeItem, uintptr_t>>& removed);
    std::unordered_map<TreeItem, Node> nodes_;
    std::vector<TreeItem> roots_;
    TreeItem next_ = 1, selection_ = 0;
    mutable bool dirty_ = true;
    mutable std::vector<Row> rows_;
};
}
