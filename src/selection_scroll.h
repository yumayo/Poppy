#pragma once
#include <algorithm>

namespace poppy {
// Scroll visual rows while a captured selection is outside the text viewport.
// Distance controls speed, capped so a far-away pointer remains manageable.
inline int selectionScrollRows(int y, int top, int bottom, int rowHeight) {
    if (bottom <= top || rowHeight <= 0) return 0;
    if (y < top) return -std::min(8, 1 + (top - 1 - y) / rowHeight);
    if (y >= bottom) return std::min(8, 1 + (y - bottom) / rowHeight);
    return 0;
}
}
