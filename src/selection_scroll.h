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
// Continuous form of selectionScrollRows for pixel scrolling: signed units
// per second, growing with the pointer's distance outside [top, bottom).
// The base rate matches one unit per 50 ms tick, the old timer interval.
inline double selectionScrollSpeed(int y, int top, int bottom, int unit) {
    if (bottom <= top || unit <= 0) return 0;
    const int distance = y < top ? top - 1 - y : y >= bottom ? y - bottom : -1;
    if (distance < 0) return 0;
    const double speed = 20.0 * std::min(8.0, 1.0 + static_cast<double>(distance) / unit);
    return y < top ? -speed : speed;
}
}
