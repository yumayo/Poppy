#include "tree_model.h"
#include <algorithm>

namespace poppy {
TreeItem TreeModel::insert(TreeItem parent, std::wstring text, bool expandable, uintptr_t data, uint32_t color) {
    if (parent && !get(parent)) return 0;
    const auto item = next_++;
    nodes_.emplace(item, Node{parent, std::move(text), data, color, expandable, false, {}});
    auto& siblings = parent ? nodes_.at(parent).children : roots_;
    if (!siblings.empty()) nodes_.at(siblings.back()).next = item;
    siblings.push_back(item);
    dirty_ = true; return item;
}
const TreeModel::Node* TreeModel::get(TreeItem item) const {
    const auto found = nodes_.find(item); return found == nodes_.end() ? nullptr : &found->second;
}
void TreeModel::label(TreeItem item, std::wstring text, uint32_t color) {
    const auto found = nodes_.find(item);
    if (found != nodes_.end()) { found->second.text = std::move(text); found->second.color = color; }
}
bool TreeModel::descendant(TreeItem item, TreeItem ancestor) const {
    for (auto current = parent(item); current; current = parent(current)) if (current == ancestor) return true;
    return false;
}
bool TreeModel::reparent(TreeItem item, TreeItem parent) {
    const auto node = get(item), target = get(parent);
    if (!node || !node->parent || !target || !target->expandable || node->parent == parent || item == parent || descendant(parent, item)) return false;
    auto& siblings = nodes_.at(node->parent).children;
    const auto found = std::find(siblings.begin(), siblings.end(), item);
    if (found != siblings.begin()) nodes_.at(*(found - 1)).next = node->next;
    siblings.erase(found);
    auto& children = nodes_.at(parent).children;
    if (!children.empty()) nodes_.at(children.back()).next = item;
    children.push_back(item);
    nodes_.at(item).parent = parent; nodes_.at(item).next = 0;
    dirty_ = true; return true;
}
bool TreeModel::expand(TreeItem item, bool expanded) {
    const auto found = nodes_.find(item);
    if (found == nodes_.end() || !found->second.expandable || found->second.expanded == expanded) return false;
    found->second.expanded = expanded;
    if (!expanded) {
        bool hidden = false;
        for (auto it = selected_.begin(); it != selected_.end();) {
            if (descendant(*it, item)) { it = selected_.erase(it); hidden = true; }
            else ++it;
        }
        if (hidden) selected_.insert(item);
        if (descendant(selection_, item)) selection_ = item;
        if (descendant(anchor_, item)) anchor_ = item;
    }
    dirty_ = true; return true;
}
bool TreeModel::select(TreeItem item) {
    return select(item, false, false);
}
bool TreeModel::select(TreeItem item, bool toggle, bool range) {
    if (item && !get(item)) return false;
    if (range && item) {
        const auto& rows = visible();
        auto first = std::find_if(rows.begin(), rows.end(), [&](const Row& row) { return row.item == anchor_; });
        auto last = std::find_if(rows.begin(), rows.end(), [&](const Row& row) { return row.item == item; });
        if (last == rows.end()) return false;
        if (first == rows.end()) { anchor_ = selection_ ? selection_ : item; first = std::find_if(rows.begin(), rows.end(), [&](const Row& row) { return row.item == anchor_; }); }
        if (first == rows.end()) { first = last; anchor_ = item; }
        if (!toggle) selected_.clear();
        if (first > last) std::swap(first, last);
        for (auto it = first; it != last + 1; ++it) selected_.insert(it->item);
    } else {
        if (!toggle) selected_.clear();
        if (item && (!toggle || !selected_.erase(item))) selected_.insert(item);
        anchor_ = item;
    }
    selection_ = item; return true;
}
void TreeModel::selectAll(bool includeRoots) {
    selected_.clear();
    for (const auto& row : visible()) if (includeRoots || row.depth) selected_.insert(row.item);
    if (!selected(selection_)) {
        selection_ = 0;
        for (const auto& row : visible()) if (selected(row.item)) { selection_ = row.item; break; }
    }
    anchor_ = selection_;
}
std::vector<TreeItem> TreeModel::selections(bool rootsOnly) const {
    std::vector<TreeItem> result;
    size_t coveredDepth = static_cast<size_t>(-1);
    for (const auto& row : visible()) {
        if (rootsOnly && row.depth > coveredDepth) continue;
        coveredDepth = static_cast<size_t>(-1);
        if (selected(row.item)) {
            result.push_back(row.item);
            if (rootsOnly) coveredDepth = row.depth;
        }
    }
    return result;
}
TreeItem TreeModel::child(TreeItem parent) const {
    const auto node = get(parent);
    if (parent && !node) return 0;
    const auto& children = node ? node->children : roots_;
    return children.empty() ? 0 : children.front();
}
TreeItem TreeModel::next(TreeItem item) const {
    const auto node = get(item); return node ? node->next : 0;
}
TreeItem TreeModel::parent(TreeItem item) const { const auto node = get(item); return node ? node->parent : 0; }
const std::vector<TreeModel::Row>& TreeModel::visible() const {
    if (!dirty_) return rows_;
    rows_.clear();
    std::vector<Row> pending;
    for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) pending.push_back({*it, 0});
    while (!pending.empty()) {
        const auto row = pending.back(); pending.pop_back(); rows_.push_back(row);
        const auto& node = nodes_.at(row.item);
        if (node.expanded) for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) pending.push_back({*it, row.depth + 1});
    }
    dirty_ = false; return rows_;
}
std::vector<std::pair<TreeItem, uintptr_t>> TreeModel::erase(TreeItem item) {
    const auto node = get(item); if (!node) return {};
    const auto parent = node->parent;
    const bool focusRemoved = selection_ == item || descendant(selection_, item);
    if (focusRemoved) selection_ = parent;
    if (anchor_ == item || descendant(anchor_, item)) anchor_ = parent;
    auto& siblings = parent ? nodes_.at(parent).children : roots_;
    const auto found = std::find(siblings.begin(), siblings.end(), item);
    if (found != siblings.begin()) nodes_.at(*(found - 1)).next = node->next;
    siblings.erase(found);
    std::vector<std::pair<TreeItem, uintptr_t>> removed;
    eraseSubtree(item, removed);
    if (focusRemoved && selected_.empty() && parent) selected_.insert(parent);
    dirty_ = true; return removed;
}
void TreeModel::eraseSubtree(TreeItem item, std::vector<std::pair<TreeItem, uintptr_t>>& removed) {
    std::vector<TreeItem> pending{item};
    while (!pending.empty()) {
        const auto current = pending.back(); pending.pop_back();
        const auto found = nodes_.find(current);
        pending.insert(pending.end(), found->second.children.begin(), found->second.children.end());
        selected_.erase(current);
        removed.emplace_back(current, found->second.data); nodes_.erase(found);
    }
}
std::vector<std::pair<TreeItem, uintptr_t>> TreeModel::eraseChildren(TreeItem parent) {
    const auto found = nodes_.find(parent); if (found == nodes_.end()) return {};
    const bool focusRemoved = descendant(selection_, parent);
    if (focusRemoved) selection_ = parent;
    if (descendant(anchor_, parent)) anchor_ = parent;
    std::vector<std::pair<TreeItem, uintptr_t>> removed;
    for (const auto child : found->second.children) eraseSubtree(child, removed);
    if (focusRemoved && selected_.empty()) selected_.insert(parent);
    found->second.children.clear(); dirty_ = true; return removed;
}
std::vector<std::pair<TreeItem, uintptr_t>> TreeModel::clear() {
    std::vector<std::pair<TreeItem, uintptr_t>> removed; removed.reserve(nodes_.size());
    for (const auto& pair : nodes_) removed.emplace_back(pair.first, pair.second.data);
    nodes_.clear(); roots_.clear(); rows_.clear(); selected_.clear(); selection_ = anchor_ = 0; dirty_ = true; return removed;
}
void TreeModel::sort(TreeItem parent, const std::function<bool(uintptr_t, uintptr_t)>& compare) {
    const auto found = nodes_.find(parent);
    if (parent && found == nodes_.end()) return;
    auto& children = parent ? found->second.children : roots_;
    std::stable_sort(children.begin(), children.end(), [&](TreeItem a, TreeItem b) { return compare(nodes_.at(a).data, nodes_.at(b).data); });
    for (size_t i = 0; i < children.size(); ++i) nodes_.at(children[i]).next = i + 1 < children.size() ? children[i + 1] : 0;
    dirty_ = true;
}
}
