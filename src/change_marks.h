#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace poppy {
struct ChangeMark { int top = 0, bottom = 0; };
struct ChangeMarkViewport { size_t begin = 0, end = 0; int top = 0, bottom = 0; };
// Map visual row boundaries to the overview strip. When the scrollbar enlarges
// its thumb, expand the visible rows to match it and compress either side.
// Keep isolated changes and EOF deletions visible even in very large files.
inline ChangeMark changeMark(size_t begin, size_t end, size_t rows, int top, int bottom, int minimum, ChangeMarkViewport viewport = {}) {
    if (!rows || bottom <= top) return {top, top};
    begin = std::min(begin, rows); end = std::clamp(end, begin, rows);
    const bool hasViewport = viewport.begin < viewport.end && viewport.end <= rows &&
        top <= viewport.top && viewport.top < viewport.bottom && viewport.bottom <= bottom;
    const auto project = [&](size_t row, bool roundUp) {
        size_t first = 0, last = rows;
        int upper = top, lower = bottom;
        if (hasViewport) {
            if (row < viewport.begin) { last = viewport.begin; lower = viewport.top; }
            else if (row < viewport.end) { first = viewport.begin; last = viewport.end; upper = viewport.top; lower = viewport.bottom; }
            else { first = viewport.end; upper = viewport.bottom; }
        }
        if (first == last) return lower;
        const auto pixels = static_cast<uint64_t>(row - first) * (lower - upper);
        return upper + static_cast<int>((pixels + (roundUp ? last - first - 1 : 0)) / (last - first));
    };
    const int first = project(begin, false), last = project(end, true);
    int upper = top, lower = bottom;
    // Minimum-height marks must stay on their side of the viewport boundary;
    // otherwise a nearby offscreen change can still appear inside the thumb.
    if (hasViewport) {
        if (begin >= viewport.end && viewport.end < rows) upper = viewport.bottom;
        else if (begin >= viewport.begin) upper = viewport.top;
        if (end <= viewport.begin && begin < viewport.begin) lower = viewport.top;
        else if (end <= viewport.end && (begin < viewport.end || viewport.end == rows)) lower = viewport.bottom;
    }
    const int size = std::min(lower - upper, std::max(std::max(1, minimum), last - first));
    const int start = std::clamp(first, upper, lower - size);
    return {start, start + size};
}
}
