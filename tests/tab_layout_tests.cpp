#include "tab_layout.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void rowWidths() {
    // 136px for Split/Inline and 58px for terminal controls on the top row.
    const auto layout = layoutTabs({{150}, {150}, {200}, {250}}, 500, 306, 56, 5);
    check(layout.rows == 2 && !layout.overflow(), "Controls on one row caused unnecessary wrapping on another");
    check(layout.tabs[1].right == 300 && layout.tabs[2].row == 1 && layout.tabs[3].right == 450,
        "Tabs did not use the space below Split/Inline");
    const auto wide = layoutTabs({{300}, {480}}, 500, 306, 56, 5);
    check(wide.tabs[1].row == 1 && wide.tabs[1].right == 480, "A wrapped tab retained the previous row's width limit");
    const auto pinned = layoutTabs({{250, true}, {480}}, 500, 306, 56, 5);
    check(pinned.rows == 2 && pinned.tabs[1].right == 480, "The normal row below pinned tabs reserved top-row controls");
    const auto single = layoutTabs({{900}}, 500, 306, 56, 1);
    check(single.tabs[0].right == 306 && !single.overflow(), "A single row overlapped controls or reserved unnecessary paging");
}
void paging() {
    const std::vector<TabSize> tabs(12, {150});
    auto layout = layoutTabs(tabs, 500, 306, 56, 3);
    check(layout.overflow() && layout.rows == 3 && layout.tabs[4].row == 1 && layout.tabs[4].right == 450,
        "Paging reduced the width of an unobstructed middle row");
    check(layout.tabs[6].row == 2 && layout.tabs[6].right == 300 && layout.tabs[7].row == 3,
        "The last visible row overlapped paging controls");
    const auto next = layoutTabs(tabs, 500, 306, 56, 3, 1);
    check(next.firstRow == 1 && next.starts[next.firstRow] == 2 && next.tabs[3].right == 300,
        "Scrolling did not reserve controls on the new first visible row");
    const auto back = layoutTabs(tabs, 500, 306, 56, 3, next.firstRow - 1);
    check(back.starts == layout.starts, "Scrolling back changed the initial row boundaries");
    const auto bottom = layoutTabs(tabs, 500, 306, 56, 3, 0, tabs.size() - 1);
    check(bottom.tabs.back().row >= bottom.firstRow && bottom.tabs.back().row < bottom.firstRow + bottom.rows,
        "Revealing the last tab failed after reflowing visible rows");
    const auto top = layoutTabs(tabs, 500, 306, 56, 3, bottom.firstRow, 0);
    check(top.firstRow == 0, "Revealing the first tab did not scroll back");
    layout = layoutTabs(tabs, 500, 306, 56, 1, 0, 8);
    check(layout.tabs[8].row == layout.firstRow && layout.tabs[8].right <= 250,
        "Single-row paging overlapped Split/Inline or failed to reveal a tab");
    const auto resized = layoutTabs(tabs, 3000, 2806, 56, 3, bottom.firstRow);
    check(!resized.overflow() && resized.firstRow == 0 && resized.rows == 1, "Widening retained stale paging or hidden rows");
    const auto closed = layoutTabs({{150}, {150}}, 500, 306, 56, 3, bottom.firstRow);
    check(!closed.overflow() && closed.firstRow == 0 && closed.rows == 1, "Closing tabs retained a scrolled viewport");
}
void geometry() {
    for (int scale : {1, 2, 3}) for (int width : {0, 20, 58, 160, 300, 500, 1000}) {
        const int full = width * scale, top = std::max(0, width - 194) * scale;
        const int pager = 56 * scale;
        std::vector<TabSize> tabs;
        for (int i = 0; i < 35; ++i) tabs.push_back({(60 + (i * 73) % 650) * scale, i < 4});
        for (size_t rows : {1, 2, 3, 8}) for (size_t selected = 0; selected < tabs.size(); ++selected) {
            const auto layout = layoutTabs(tabs, full, top, pager, rows, 99, selected);
            check(layout.tabs.size() == tabs.size() && layout.firstRow < layout.starts.size() && layout.rows <= rows, "Invalid viewport bounds");
            const auto active = layout.tabs[selected];
            check(active.row >= layout.firstRow && active.row < layout.firstRow + layout.rows, "The selected tab remained hidden");
            for (size_t i = 0; i < tabs.size(); ++i) {
                const auto tab = layout.tabs[i];
                check(tab.left >= 0 && tab.right >= tab.left && tab.right <= full, "A tab escaped its pane");
                if (tab.row == layout.firstRow) check(tab.right <= top, "A visible tab overlapped top-row controls");
                if (layout.overflow() && tab.row == layout.firstRow + layout.rows - 1)
                    check(tab.right <= std::max(0, (rows == 1 ? top : full) - pager), "A visible tab overlapped paging");
                if (i && layout.tabs[i - 1].row == tab.row)
                    check(layout.tabs[i - 1].right == tab.left && tabs[i - 1].pinned == tabs[i].pinned,
                        "Tabs overlapped or mixed pinned and normal rows");
            }
            const auto repeat = layoutTabs(tabs, full, top, pager, rows, layout.firstRow, selected);
            check(repeat.firstRow == layout.firstRow && repeat.starts == layout.starts, "Repeating layout moved the viewport");
        }
    }
    const auto empty = layoutTabs({}, 500, 306, 56, 0, 99);
    check(empty.tabs.empty() && empty.starts.empty() && !empty.rows && !empty.firstRow, "An empty strip retained stale rows");
}
void scrolling() {
    std::vector<TabSize> tabs;
    for (int i = 0; i < 50; ++i) tabs.push_back({70 + (i * 73) % 650, i < 4});
    for (int width : {50, 200, 500, 1000}) for (int reserved : {0, 58, 194, 218}) for (size_t rows : {1, 2, 3, 8}) {
        const int top = std::max(0, width - reserved);
        auto layout = layoutTabs(tabs, width, top, 56, rows);
        std::vector<bool> seen(tabs.size());
        for (size_t steps = 0;; ++steps) {
            check(steps < tabs.size(), "Scrolling failed to reach the last page");
            for (size_t i = 0; i < tabs.size(); ++i)
                if (layout.tabs[i].row >= layout.firstRow && layout.tabs[i].row < layout.firstRow + layout.rows) seen[i] = true;
            if (layout.firstRow + layout.rows >= layout.starts.size()) break;
            const auto next = layoutTabs(tabs, width, top, 56, rows, layout.firstRow + 1);
            if (next.firstRow <= layout.firstRow) throw std::runtime_error("Next-page button did not advance: width=" + std::to_string(width) +
                ", reserved=" + std::to_string(reserved) + ", rows=" + std::to_string(rows) + ", first=" + std::to_string(layout.firstRow) +
                ", total=" + std::to_string(layout.starts.size()) + ", next total=" + std::to_string(next.starts.size()));
            layout = next;
        }
        check(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }), "Paging skipped tabs");
        for (size_t steps = 0; layout.firstRow; ++steps) {
            check(steps < tabs.size(), "Scrolling failed to return to the first page");
            const auto previous = layoutTabs(tabs, width, top, 56, rows, layout.firstRow - 1);
            check(previous.firstRow < layout.firstRow, "An enabled previous-page button did not move back");
            layout = previous;
        }
    }
}
}
int main() {
    try { rowWidths(); paging(); geometry(); scrolling(); std::cout << "Tab layout tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
