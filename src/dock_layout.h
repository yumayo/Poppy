#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
struct ProjectSession;
enum class DockSide { Center, Left, Right, Top, Bottom };
struct DockRect { int left = 0, top = 0, right = 0, bottom = 0; };
struct DockNode {
    int group = 0, first = -1, second = -1, ratio = 500;
    bool horizontal = false; // true: top/bottom; false: left/right
    std::vector<std::string> tabs;
    std::string active;
    bool leaf() const { return first < 0; }
};
struct DockPlacement { int node, group; DockRect rect; };
struct DockDivider { int node; bool horizontal; DockRect rect, area; };
struct DockGeometry { std::vector<DockPlacement> panes; std::vector<DockDivider> dividers; };
class DockLayout {
public:
    static constexpr int maxGroups = 1024;
    DockLayout();
    const std::vector<DockNode>& nodes() const { return nodes_; }
    DockNode* pane(int group);
    const DockNode* pane(int group) const;
    int split(int group, DockSide side);
    void retain(const std::vector<int>& groups);
    void resize(int node, int ratio);
    DockGeometry arrange(DockRect area, int divider, bool lowerVisible = true) const;
    std::string save() const;
    bool restore(std::string_view data);
    static DockSide dropSide(DockRect area, int x, int y);
    static DockRect preview(DockRect area, DockSide side);
private:
    std::vector<DockNode> nodes_;
};
// Upgrade the previous separate editor/terminal splits on first use.
DockLayout restoreDockLayout(ProjectSession& state, int height);
}
