#pragma once
#include <algorithm>
#include <cstddef>

namespace poppy {
struct ChangeMark { double top = 0, bottom = 0; };
// Magnification keeps ordinary offscreen marks away from the thumb center.
// Retain a center guard for zero-length source deletions (no highlighted text)
// and tracks too short for the minimum marker height.
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
        return {static_cast<double>(top + quarter), static_cast<double>(bottom - quarter)};
    }
};
struct ChangeMarkViewport { size_t begin = 0, end = 0; int thumb = 0; };
// Magnify the visible rows without using the scrollbar's rounded position.
// For track T, thumb H, total rows N and page P, the outside scale is
// a=(T-H)/(N-P). Add (H-a*P)*clamp((row-first)/P,0,1) to a*row.
// Both outside regions stay fixed; inside, each boundary moves continuously
// in one direction as first increases. Keep fractional coordinates through
// painting, so pixel rounding cannot reverse that motion or change heights.
inline ChangeMark changeMark(size_t begin, size_t end, size_t rows, int top, int bottom, int minimum, ChangeMarkViewport viewport = {}) {
    if (!rows || bottom <= top) return {static_cast<double>(top), static_cast<double>(top)};
    begin = std::min(begin, rows); end = std::clamp(end, begin, rows);
    const double height = bottom - top;
    const size_t page = std::min(rows, viewport.end > viewport.begin ? viewport.end - viewport.begin : 0);
    const size_t first = std::min(viewport.begin, rows - page);
    const double thumb = std::clamp(static_cast<double>(viewport.thumb), 0.0, height);
    const bool expand = page && page < rows && thumb > height * page / rows;
    const double scale = expand ? (height - thumb) / (rows - page) : height / rows;
    const double extra = expand ? thumb - scale * page : 0;
    const auto project = [&](size_t row) {
        const double fraction = !expand || row <= first ? 0 : row - first >= page ? 1 : static_cast<double>(row - first) / page;
        return top + scale * row + extra * fraction;
    };
    const double start = project(begin), finish = project(end);
    const double size = std::min(height, std::max(static_cast<double>(std::max(1, minimum)), finish - start));
    const double upper = std::min(start, bottom - size);
    return {upper, upper + size};
}
}
