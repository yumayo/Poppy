#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace poppy::terminalWire {
// Versioned, bounded, little-endian data. Never transfer C++ object layouts.
inline constexpr uint32_t version = 1;
inline constexpr size_t maxPacket = 32 * 1024 * 1024;
class Writer {
public:
    std::string bytes;
    void number(uint64_t value, unsigned count = 4) { for (unsigned i = 0; i < count; ++i) bytes += static_cast<char>(value >> (i * 8)); }
    void flag(bool value) { number(value, 1); }
    void string(std::string_view value) {
        if (value.size() > maxPacket || bytes.size() + value.size() + 4 > maxPacket) throw std::runtime_error("Terminal packet is too large");
        number(value.size()); bytes.append(value);
    }
};
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) { if (bytes.size() > maxPacket) invalid(); }
    uint64_t number(unsigned count = 4) {
        if (count > 8 || bytes_.size() < count) invalid();
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes_[i])) << (i * 8);
        bytes_.remove_prefix(count); return value;
    }
    uint32_t bounded(uint32_t low, uint32_t high) { const auto value = number(); if (value < low || value > high) invalid(); return static_cast<uint32_t>(value); }
    bool flag() { const auto value = number(1); if (value > 1) invalid(); return value != 0; }
    std::string string(size_t limit = maxPacket) {
        const auto size = number(); if (size > limit || size > bytes_.size()) invalid();
        std::string value(bytes_.substr(0, static_cast<size_t>(size))); bytes_.remove_prefix(static_cast<size_t>(size)); return value;
    }
    void finish() const { if (!bytes_.empty()) invalid(); }
    [[noreturn]] static void invalid() { throw std::runtime_error("Invalid terminal protocol data"); }
private:
    std::string_view bytes_;
};
}
