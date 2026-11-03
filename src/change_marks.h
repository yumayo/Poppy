#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace poppy {
struct ChangeMark { int top = 0, bottom = 0; };
// Fixed, minimum-height marks can cover the thumb center even when their
// source rows are offscreen. Mask the middle half of the thumb unless the
// viewport contains a highlighted change. The outer quarters remain visible.
struct ChangeMarkVisibility {
    size_t begin = 0, end = 0;
    int top = 0, bottom = 0;
    bool visible = false;
    void include(size_t first, size_t last) {
        // Zero-length source deletions have no highlighted text row.
        visible = visible || (begin < end && first < last && first < end && begin < last);
    }
    ChangeMark mask() const {
        if (visible || bottom <= top) return {};
        const int quarter = (bottom - top) / 4;
        return {top + quarter, bottom - quarter};
    }
};
// Inset both ends when the thumb is larger than its proportional height.
// For track T, thumb H, total rows N and page P, use (H*N - T*P)/(2*(N-P)).
// This aligns the page center with the thumb center at every scroll position,
// without making the marker positions depend on scrolling. For N >> P this
// approaches H/2. Leave at least one pixel for marks on very short tracks.
inline int changeMarkInset(int track, int thumb, size_t rows, size_t page) {
    if (track <= 1 || thumb <= 0 || rows <= page) return 0;
    const auto enlarged = static_cast<uint64_t>(std::min(thumb, track)) * rows;
    const auto proportional = static_cast<uint64_t>(track) * page;
    if (enlarged <= proportional) return 0;
    const auto span = static_cast<uint64_t>(rows - page);
    return std::min((track - 1) / 2, static_cast<int>((enlarged - proportional + span) / (2 * span)));
}
// Map visual row boundaries to the overview strip, independently of scrolling.
// Keep isolated changes and EOF deletions visible even in very large files.
inline ChangeMark changeMark(size_t begin, size_t end, size_t rows, int top, int bottom, int minimum) {
    if (!rows || bottom <= top) return {top, top};
    const auto height = static_cast<uint64_t>(bottom - top);
    begin = std::min(begin, rows); end = std::clamp(end, begin, rows);
    const int first = top + static_cast<int>(begin * height / rows);
    const int last = top + static_cast<int>((end * height + rows - 1) / rows);
    const int size = std::min(bottom - top, std::max(std::max(1, minimum), last - first));
    const int start = std::min(first, bottom - size);
    return {start, start + size};
}
}
