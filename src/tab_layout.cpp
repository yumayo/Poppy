#include "tab_layout.h"
#include <algorithm>

namespace poppy {
TabLayout layoutTabs(const std::vector<TabSize>& tabs, int width, int firstRowWidth, int pagingWidth,
    size_t maxRows, size_t firstRow, size_t selected) {
    width = std::max(0, width);
    firstRowWidth = std::clamp(firstRowWidth, 0, width);
    pagingWidth = std::max(0, pagingWidth);
    maxRows = std::max<size_t>(1, maxRows);
    TabLayout result;
    result.tabs.resize(tabs.size());
    const auto wrap = [&](size_t first, bool paging) {
        result.starts.clear();
        size_t row = 0;
        int x = 0;
        const auto available = [&] {
            // Scrolled-out rows use the top row's capacity, so advancing one
            // row always removes exactly the tabs that fitted at the top.
            int space = row <= first ? firstRowWidth : width;
            if (paging && ((maxRows == 1 && row <= first) || row == first + maxRows - 1))
                space = std::max(0, space - pagingWidth);
            return space;
        };
        for (size_t i = 0; i < tabs.size(); ++i) {
            const int wanted = std::max(0, tabs[i].width);
            if (!i || tabs[i].pinned != tabs[i - 1].pinned || (i > result.starts.back() && wanted > available() - x)) {
                if (i) ++row;
                result.starts.push_back(i); x = 0;
            }
            const int tabWidth = std::min(wanted, available());
            result.tabs[i] = {x, x + tabWidth, row}; x += tabWidth;
        }
        result.rows = std::min(maxRows, result.starts.size());
    };
    // Check the unscrolled layout first, so widening or closing tabs can
    // remove paging even if the previous viewport started further down.
    wrap(0, false);
    if (!result.overflow()) return result;
    // Paging can create more rows than the initial layout without buttons.
    firstRow = std::min(firstRow, tabs.size() - 1);
    for (;;) {
        wrap(firstRow, true);
        // Reflow can leave fewer occupied rows on the final page. Keep the
        // viewport height while paging, otherwise clamping to a full page can
        // move back and make the last tabs unreachable.
        size_t next = std::min(firstRow, result.starts.size() - 1);
        if (selected < result.tabs.size()) {
            const auto row = result.tabs[selected].row;
            if (row < next) next = row;
            else if (row >= next + result.rows) next = row - result.rows + 1;
        }
        if (next == firstRow) break;
        firstRow = next;
    }
    result.firstRow = firstRow;
    return result;
}
}
