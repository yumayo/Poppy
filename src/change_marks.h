#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace poppy {
struct ChangeMark { int top = 0, bottom = 0; };
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
