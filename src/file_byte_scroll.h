#pragma once
#include <algorithm>
#include <cstdint>

namespace poppy {
// A byte fraction fits Win32's signed scroll positions even for multi-TB files.
inline uint64_t scaleFileBytes(uint64_t value, uint64_t source, uint64_t destination) {
    if (!source || !value) return 0;
    if (value >= source) return destination;
    const auto scaled = static_cast<long double>(value) * destination / source;
    return scaled >= static_cast<long double>(destination) ? destination : static_cast<uint64_t>(scaled);
}
struct FileByteScroll {
    uint64_t bytes = 0, viewport = 0;
    int range() const { return static_cast<int>(std::clamp<uint64_t>(bytes, 1, 1000000000)); }
    int page() const { return static_cast<int>(std::max<uint64_t>(1, scaleFileBytes(viewport, bytes, range()))); }
    int maximum() const { return range() - page(); }
    int position(uint64_t byte) const { return static_cast<int>(scaleFileBytes(byte, bytes, maximum())); }
    uint64_t byte(int position) const { return scaleFileBytes(static_cast<uint64_t>(std::max(0, position)), maximum(), bytes); }
};
}
