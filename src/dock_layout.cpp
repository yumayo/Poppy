#include "dock_layout.h"
#include "session.h"
#include <algorithm>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>

namespace poppy {
DockLayout::DockLayout() : nodes_(1) {}
DockNode* DockLayout::pane(int group) {
    for (auto& node : nodes_) if (node.leaf() && node.group == group) return &node;
    return nullptr;
}
const DockNode* DockLayout::pane(int group) const {
    for (const auto& node : nodes_) if (node.leaf() && node.group == group) return &node;
    return nullptr;
}
int DockLayout::split(int group, DockSide side) {
    if (side == DockSide::Center) return group;
    const auto original = pane(group);
    if (!original) return -1;
    int next = 0;
    // Reuse free IDs so repeated splits and closes do not accumulate viewers.
    while (pane(next)) ++next;
    if (next >= maxGroups) return -1;
    const auto index = static_cast<size_t>(original - nodes_.data());
    auto previous = *original;
    const int a = static_cast<int>(nodes_.size()), b = a + 1;
    DockNode added; added.group = next;
    const bool before = side == DockSide::Left || side == DockSide::Top;
    nodes_[index] = {}; nodes_[index].group = -1;
    nodes_[index].horizontal = side == DockSide::Top || side == DockSide::Bottom;
    nodes_[index].first = a; nodes_[index].second = b;
    nodes_.push_back(before ? added : previous); nodes_.push_back(before ? previous : added);
    return next;
}
void DockLayout::retain(const std::vector<int>& groups) {
    if (groups.empty()) { nodes_.assign(1, DockNode{}); return; }
    std::vector<DockNode> compact;
    const auto visit = [&](const auto& self, int index) -> int {
        auto node = nodes_[index];
        if (node.leaf()) {
            if (std::find(groups.begin(), groups.end(), node.group) == groups.end()) return -1;
        } else {
            node.first = self(self, node.first); node.second = self(self, node.second);
            if (node.first < 0) return node.second;
            if (node.second < 0) return node.first;
        }
        compact.push_back(std::move(node)); return static_cast<int>(compact.size() - 1);
    };
    const int root = visit(visit, 0);
    if (root < 0) { nodes_.assign(1, DockNode{}); return; }
    // Rebuild in preorder, keeping root at zero and no unreachable nodes.
    std::vector<DockNode> ordered;
    const auto copy = [&](const auto& self, int index) -> int {
        const int result = static_cast<int>(ordered.size()); ordered.push_back(compact[index]);
        if (!compact[index].leaf()) {
            const int first = self(self, compact[index].first), second = self(self, compact[index].second);
            ordered[result].first = first; ordered[result].second = second;
        }
        return result;
    };
    copy(copy, root); nodes_ = std::move(ordered);
}
void DockLayout::resize(int node, int ratio) {
    if (node >= 0 && node < static_cast<int>(nodes_.size()) && !nodes_[node].leaf()) nodes_[node].ratio = std::clamp(ratio, 50, 950);
}
DockGeometry DockLayout::arrange(DockRect area, int divider, bool lowerVisible) const {
    DockGeometry result;
    area.right = std::max(area.left, area.right); area.bottom = std::max(area.top, area.bottom);
    const auto visit = [&](const auto& self, int index, DockRect rect, bool collapse) -> void {
        const auto& node = nodes_[index];
        if (node.leaf()) { result.panes.push_back({index, node.group, rect}); return; }
        if (node.horizontal && collapse) { self(self, node.first, rect, false); return; }
        const int extent = node.horizontal ? rect.bottom - rect.top : rect.right - rect.left;
        const int gap = std::clamp(divider, 0, extent), available = extent - gap;
        const int minimum = std::min(80, available / 2);
        const int length = std::clamp(static_cast<int>(static_cast<long long>(available) * node.ratio / 1000), minimum, available - minimum);
        auto first = rect, second = rect, bar = rect;
        if (node.horizontal) { first.bottom = rect.top + length; second.top = first.bottom + gap; bar.top = first.bottom; bar.bottom = second.top; }
        else { first.right = rect.left + length; second.left = first.right + gap; bar.left = first.right; bar.right = second.left; }
        result.dividers.push_back({index, node.horizontal, bar, rect});
        self(self, node.first, first, collapse); self(self, node.second, second, collapse);
    };
    visit(visit, 0, area, !lowerVisible); return result;
}
std::string DockLayout::save() const {
    std::ostringstream out; out << "1 ";
    const auto write = [&](const auto& self, int index) -> void {
        const auto& n = nodes_[index];
        if (n.leaf()) {
            out << "L " << n.group << ' ' << std::quoted(n.active) << ' ' << n.tabs.size() << ' ';
            for (const auto& tab : n.tabs) out << std::quoted(tab) << ' ';
        } else { out << (n.horizontal ? "H " : "V ") << n.ratio << ' '; self(self, n.first); self(self, n.second); }
    };
    write(write, 0); return out.str();
}
bool DockLayout::restore(std::string_view data) {
    if (data.empty() || data.size() > 16 * 1024 * 1024) return false;
    std::istringstream in{std::string(data)}; int version = 0; if (!(in >> version) || version != 1) return false;
    std::vector<DockNode> parsed; std::set<int> groups; size_t tabCount = 0;
    const auto read = [&](const auto& self, int depth) -> int {
        if (depth >= maxGroups || parsed.size() >= maxGroups * 2 - 1) return -1;
        char kind = 0; if (!(in >> kind)) return -1;
        DockNode node; const int index = static_cast<int>(parsed.size()); parsed.push_back(node);
        if (kind == 'L') {
            size_t count = 0;
            if (!(in >> node.group >> std::quoted(node.active) >> count) || node.group < 0 || node.group >= maxGroups ||
                node.active.size() > 65536 || !groups.insert(node.group).second || count > 10100 - tabCount) return -1;
            tabCount += count;
            for (size_t i = 0; i < count; ++i) { std::string key; if (!(in >> std::quoted(key)) || key.size() > 65536) return -1; node.tabs.push_back(std::move(key)); }
        } else if (kind == 'H' || kind == 'V') {
            node.group = -1; node.horizontal = kind == 'H';
            if (!(in >> node.ratio) || node.ratio < 50 || node.ratio > 950) return -1;
            node.first = self(self, depth + 1); if (node.first < 0) return -1;
            node.second = self(self, depth + 1); if (node.second < 0) return -1;
        } else return -1;
        parsed[index] = std::move(node); return index;
    };
    if (read(read, 0) < 0) return false;
    in >> std::ws; if (!in.eof()) return false;
    nodes_ = std::move(parsed); return true;
}
DockSide DockLayout::dropSide(DockRect r, int x, int y) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return DockSide::Center;
    const int dx = std::min(x - r.left, r.right - x), dy = std::min(y - r.top, r.bottom - y);
    if (dx * 4 >= w && dy * 4 >= h) return DockSide::Center;
    if (static_cast<long long>(dx) * h < static_cast<long long>(dy) * w) return x < (r.left + r.right) / 2 ? DockSide::Left : DockSide::Right;
    return y < (r.top + r.bottom) / 2 ? DockSide::Top : DockSide::Bottom;
}
DockRect DockLayout::preview(DockRect r, DockSide side) {
    switch (side) {
    case DockSide::Left: r.right = (r.left + r.right) / 2; break;
    case DockSide::Right: r.left = (r.left + r.right) / 2; break;
    case DockSide::Top: r.bottom = (r.top + r.bottom) / 2; break;
    case DockSide::Bottom: r.top = (r.top + r.bottom) / 2; break;
    default: break;
    }
    return r;
}
DockLayout restoreDockLayout(ProjectSession& state, int height) {
    DockLayout dock;
    if (dock.restore(state.dockLayout)) return dock;
    const int lower = dock.split(0, DockSide::Bottom);
    int right = 0, lowerRight = lower;
    if (std::any_of(state.tabs.begin(), state.tabs.end(), [](const SavedTab& tab) { return tab.group == 1; })) {
        const auto index = static_cast<int>(dock.pane(0) - dock.nodes().data());
        right = dock.split(0, DockSide::Right); dock.resize(index, state.editorRatio);
    }
    if (std::any_of(state.terminals.begin(), state.terminals.end(), [](const SavedTerminal& tab) { return tab.group == 1; })) {
        const auto index = static_cast<int>(dock.pane(lower) - dock.nodes().data());
        lowerRight = dock.split(lower, DockSide::Right); dock.resize(index, state.terminalRatio);
    }
    for (auto& tab : state.tabs) tab.group = tab.group == 1 ? right : 0;
    for (auto& terminal : state.terminals) terminal.group = terminal.group == 1 ? lowerRight : lower;
    if (height <= 0) height = 800;
    dock.resize(0, static_cast<int>((static_cast<long long>(height) - state.terminalHeight) * 1000 / height));
    return dock;
}

}
