#pragma once
#include <cstddef>
#include <vector>

namespace poppy {
struct TabSize { int width; bool pinned = false; };
struct TabPosition { int left, right; size_t row; };
struct TabLayout {
    std::vector<TabPosition> tabs;
    std::vector<size_t> starts;
    size_t firstRow = 0, rows = 0;
    bool overflow() const { return starts.size() > rows; }
};

// Controls occupy the first visible row; paging occupies the last visible row.
TabLayout layoutTabs(const std::vector<TabSize>& tabs, int width, int firstRowWidth, int pagingWidth,
    size_t maxRows, size_t firstRow = 0, size_t selected = static_cast<size_t>(-1));
}
