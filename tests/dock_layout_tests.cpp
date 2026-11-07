#include "dock_layout.h"
#include "editor_tabs.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
const DockPlacement& find(const DockGeometry& geometry, int group) {
    const auto pane = std::find_if(geometry.panes.begin(), geometry.panes.end(), [group](const auto& p) { return p.group == group; });
    if (pane == geometry.panes.end()) throw std::runtime_error("Missing dock pane");
    return *pane;
}
void nestedLayout() {
    DockLayout dock;
    const int lower = dock.split(0, DockSide::Bottom);
    const int right = dock.split(lower, DockSide::Right);
    const int bottomRight = dock.split(right, DockSide::Bottom);
    const int topRight = dock.split(0, DockSide::Right);
    const int bottomRightLeft = dock.split(bottomRight, DockSide::Left);
    check(bottomRightLeft > 4, "Docking is limited to four regions");
    auto geometry = dock.arrange({100, 40, 1300, 940}, 6);
    check(geometry.panes.size() == 6 && geometry.dividers.size() == 5, "Nested splits lost panes or dividers");
    const auto a = find(geometry, 0).rect, b = find(geometry, topRight).rect;
    const auto c = find(geometry, lower).rect, d = find(geometry, right).rect;
    const auto e = find(geometry, bottomRight).rect, f = find(geometry, bottomRightLeft).rect;
    check(a.top == 40 && a.left == 100 && a.right + 6 == b.left && b.right == 1300, "Top split has the wrong bounds");
    check(c.top == a.bottom + 6 && c.right + 6 == d.left && d.bottom + 6 == e.top, "Lower-right vertical subdivision is misplaced");
    check(f.right + 6 == e.left && f.bottom == 940 && e.right == 1300, "Nested left split is misplaced");
    const auto collapsed = dock.arrange({100, 40, 1300, 940}, 6, false);
    check(collapsed.panes.size() == 2 && find(collapsed, 0).rect.bottom == 940, "Ctrl+J did not collapse the lower subtree");
    check(dock.arrange({100, 40, 1300, 940}, 6).panes.size() == 6, "Showing the lower region lost nested layout");
    const auto divider = geometry.dividers.front(); dock.resize(divider.node, 700);
    geometry = dock.arrange({100, 40, 1300, 940}, 6);
    check(find(geometry, 0).rect.bottom > a.bottom, "Dragging a divider did not change its split ratio");
    // Every rectangle must remain valid even in a tiny/minimized window.
    for (int width : {0, 1, 5, 20, 800}) for (int height : {0, 1, 5, 25, 600}) {
        const auto small = dock.arrange({0, 0, width, height}, 8);
        for (const auto& p : small.panes) check(p.rect.left >= 0 && p.rect.top >= 0 && p.rect.right <= width && p.rect.bottom <= height && p.rect.left <= p.rect.right && p.rect.top <= p.rect.bottom, "A small window produced invalid bounds");
    }
    const int upperBottom = dock.split(0, DockSide::Bottom);
    const auto upperCollapsed = dock.arrange({0, 0, 1200, 900}, 6, false);
    check(upperCollapsed.panes.size() == 3 && find(upperCollapsed, upperBottom).rect.bottom == 900,
          "Ctrl+J collapsed a subdivision inside the retained upper subtree");
    dock.retain({topRight, bottomRight});
    check(dock.arrange({0, 0, 1000, 800}, 5).panes.size() == 2 && dock.pane(bottomRight), "Closing leaves did not collapse empty branches");
    dock.retain({bottomRight});
    geometry = dock.arrange({0, 0, 1000, 800}, 5);
    check(geometry.dividers.empty() && geometry.panes[0].rect.right == 1000 && dock.pane(bottomRight), "A sole surviving leaf lost its identity or full extent");
    check(dock.split(bottomRight, DockSide::Right) == 0, "Closed group IDs are not reused");
}
void persistence() {
    DockLayout dock;
    const int lower = dock.split(0, DockSide::Bottom);
    const int right = dock.split(lower, DockSide::Right);
    const int bottom = dock.split(right, DockSide::Bottom);
    dock.pane(0)->tabs = {"file\n日本語.cpp", "terminal:shell-1", "file \"quoted\" \\name"};
    dock.pane(0)->active = "terminal:shell-1";
    dock.pane(bottom)->tabs = {"terminal:shell-2", "diff\nfile"}; dock.pane(bottom)->active = "diff\nfile";
    dock.resize(0, 620);
    const auto saved = dock.save(); DockLayout restored;
    check(restored.restore(saved) && restored.save() == saved, "Mixed tab order, selection or split layout did not round-trip");
    const auto previous = restored.save();
    for (const auto invalid : {"", "2 L 0 \"\" 0", "1 H 500 L 0 \"\" 0", "1 V 500 L 0 \"\" 0 L 0 \"\" 0", "1 L -1 \"\" 0", "1 L 1024 \"\" 0", "1 L 0 \"\" 10101", "1 H 0 L 0 \"\" 0 L 1 \"\" 0", "1 L 0 \"\" 0 extra"}) {
        check(!restored.restore(invalid) && restored.save() == previous, "Invalid persistence partially replaced a working layout");
    }
    // Repeated subdivisions and removals must not accumulate unreachable nodes.
    for (int i = 0; i < 100; ++i) {
        const int next = restored.split(bottom, DockSide::Top);
        check(next >= 0, "Repeated splitting exhausted group IDs");
        restored.retain({0, lower, right, bottom});
        check(restored.nodes().size() == 7, "Removed split nodes accumulated");
    }
}
void migration() {
    ProjectSession state;
    state.tabs.resize(2); state.tabs[0].group = 0; state.tabs[1].group = 1;
    state.terminals.resize(2); state.terminals[0].group = 0; state.terminals[1].group = 1;
    state.editorRatio = 350; state.terminalRatio = 650; state.terminalHeight = 200;
    const auto dock = restoreDockLayout(state, 800);
    const auto geometry = dock.arrange({0, 0, 1000, 800}, 5);
    check(geometry.panes.size() == 4 && state.tabs[0].group == 0 && state.tabs[1].group == 2 &&
          state.terminals[0].group == 1 && state.terminals[1].group == 3, "Legacy editor and terminal groups were mixed during migration");
    check(find(geometry, 0).rect.right < find(geometry, 1).rect.right && find(geometry, 0).rect.bottom < find(geometry, 1).rect.top,
          "Legacy independent ratios or vertical order were lost");
    ProjectSession unopened;
    const auto deferred = restoreDockLayout(unopened, 0).arrange({0, 0, 1000, 800}, 5);
    check(find(deferred, 0).rect.bottom > 400, "An inactive project migrated with a zero-sized editor");
    state.dockLayout = dock.save();
    const auto restored = restoreDockLayout(state, 1000);
    check(restored.save() == dock.save() && state.tabs[1].group == 2 && state.terminals[1].group == 3,
          "A new dock layout was incorrectly migrated a second time");
}
void targetsAndSession() {
    const DockRect rect{100, 200, 900, 800};
    check(DockLayout::dropSide(rect, 110, 500) == DockSide::Left && DockLayout::dropSide(rect, 890, 500) == DockSide::Right &&
          DockLayout::dropSide(rect, 500, 210) == DockSide::Top && DockLayout::dropSide(rect, 500, 790) == DockSide::Bottom &&
          DockLayout::dropSide(rect, 500, 500) == DockSide::Center, "Dock drop zones do not cover all directions and merging");
    check(DockLayout::preview(rect, DockSide::Bottom).top == 500, "Drop preview is not the destination half");
    DockLayout dock; const int right = dock.split(0, DockSide::Right); const int bottom = dock.split(right, DockSide::Bottom);
    std::vector<EditorTab> tabs(4);
    tabs[0].path = ResourceId::wsl("Ubuntu", "/project/main.cpp"); tabs[0].group = bottom; tabs[0].displayed = true;
    tabs[1].terminalId = "shell-1"; tabs[1].group = bottom;
    tabs[2].path = ResourceId::wsl("Ubuntu", "/project/preview.cpp"); tabs[2].group = right; tabs[2].preview = true;
    tabs[3].path = ResourceId::wsl("Ubuntu", "/project/diff.cpp"); tabs[3].group = right; tabs[3].diff = true;
    ProjectSession state; state.dockLayout = dock.save(); saveEditorTabs(state, tabs, 0);
    check(state.tabs.size() == 2 && state.tabs[0].group == bottom && state.tabs[1].group == right && state.activeTab == 0,
          "Saving a mixed dock session remapped groups, persisted a terminal as a file, or kept a preview");
}
void topRowFiles() {
    DockLayout dock;
    check(dock.topGroup() == 0 && dock.topGroup(42) == 0, "An empty layout has no file destination");
    const int lower = dock.split(0, DockSide::Bottom);
    const int lowerRight = dock.split(lower, DockSide::Right);
    const int nestedLower = dock.split(lowerRight, DockSide::Bottom);
    const int right = dock.split(0, DockSide::Right);
    check(dock.topGroup(right) == right, "Opening a file discarded focus in the upper row");
    for (int group : {lower, lowerRight, nestedLower, 42})
        check(dock.topGroup(group) == 0, "A lower pane became the automatic file destination");
    const int upper = dock.split(0, DockSide::Top);
    check(dock.topGroup(0) == upper && dock.topGroup(right) == right, "The top row was chosen by group ID instead of split position");
    check(dock.topGroup(upper, {upper}) == right && dock.topGroup(right, {right}) == upper && dock.topGroup(-1, {right}) == upper && dock.topGroup(-1, {upper, right}) == -1,
          "Terminal exclusions selected a lower pane or hid an eligible top pane");
    const auto saved = dock.save(); DockLayout restored;
    check(restored.restore(saved) && restored.topGroup(nestedLower) == upper, "Restoring a layout changed the file destination");

    std::vector<EditorTab> tabs(2);
    tabs[0].terminalId = "shell-1"; tabs[0].group = lower; tabs[0].displayed = true;
    tabs[1].path = ResourceId::wsl("Ubuntu", "/project/moved.txt"); tabs[1].group = nestedLower; tabs[1].preview = true;
    for (auto kind : {GitDiffKind::Automatic, GitDiffKind::Changes, GitDiffKind::Staged, GitDiffKind::Commit}) {
        EditorTab target; target.path = ResourceId::wsl("Ubuntu", "/project/new.txt"); target.diffKind = kind;
        const auto index = openEditorFilePreview(tabs, target, restored, lower);
        check(tabs.size() == 3 && tabs[index].group == upper && tabs[index].preview,
              "New files or diffs did not reuse the top preview");
        check(tabs[0].terminalId == "shell-1" && tabs[0].displayed && tabs[1].group == nestedLower,
              "Opening a file changed a terminal or a manually placed preview");
    }
    auto existing = tabs[1];
    check(openEditorFilePreview(tabs, existing, restored, lower) == 1 && tabs[1].group == nestedLower,
          "Reopening a manually placed file moved it to the top row");
    restored.retain({right, nestedLower});
    check(restored.topGroup(nestedLower) == right, "Removing the upper-left pane left a stale file destination");
    restored.retain({nestedLower});
    check(restored.topGroup() == nestedLower, "A sole remaining pane was not used for files");
}
void terminalFileDestination() {
    DockLayout dock; const int right = dock.split(0, DockSide::Right);
    std::vector<EditorTab> tabs(1);
    tabs[0].terminalId = "shell-1"; tabs[0].group = 0; tabs[0].displayed = true;
    EditorTab file; file.path = ResourceId::wsl("Ubuntu", "/project/new.txt");
    const auto index = openEditorFilePreview(tabs, file, dock, 0);
    check(tabs[index].group == right && dock.nodes().size() == 3, "A top terminal was replaced despite an eligible pane beside it");
    // After closing all files, the terminal can be the only surviving pane.
    tabs.resize(1); dock.retain({0});
    const auto opened = openEditorFilePreview(tabs, file, dock, 0);
    const int upper = tabs[opened].group;
    const auto geometry = dock.arrange({0, 0, 1000, 800}, 5);
    check(upper != 0 && find(geometry, upper).rect.bottom < find(geometry, 0).rect.top && tabs[0].displayed,
          "Opening a file in a terminal-only layout did not create a pane above the terminal");
    tabs[opened].group = 0; tabs[opened].preview = false; dock.retain({0});
    const auto before = dock.save();
    check(openEditorFilePreview(tabs, file, dock, 0) == opened && dock.save() == before,
          "Selecting an existing file beside a terminal created an empty pane");
    tabs.resize(1);
    std::string full = "1 ";
    for (int group = 0; group < DockLayout::maxGroups - 1; ++group) full += "H 500 L " + std::to_string(group) + " \"\" 0 ";
    full += "L " + std::to_string(DockLayout::maxGroups - 1) + " \"\" 0 ";
    check(dock.restore(full), "Could not prepare a layout at the group limit");
    const auto fullLayout = dock.save();
    check(openEditorFilePreview(tabs, file, dock, 0) == tabs.size() && tabs.size() == 1 && dock.save() == fullLayout,
          "Reaching the group limit added a file to a terminal or changed the layout");
}
}
int main() {
    try { nestedLayout(); persistence(); migration(); targetsAndSession(); topRowFiles(); terminalFileDestination(); std::cout << "Dock layout tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
