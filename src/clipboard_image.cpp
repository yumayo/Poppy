#include "clipboard_image.h"
#include <cstdint>
#include <cwchar>
#include <stdexcept>

namespace poppy {
namespace {
uint32_t value(std::string_view bytes, size_t at, size_t count = 4) {
    if (at + count > bytes.size()) throw std::runtime_error("Truncated clipboard bitmap header.");
    uint32_t result = 0;
    for (size_t i = 0; i < count; ++i) result |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
    return result;
}
void put(std::string& bytes, size_t at, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) bytes[at + i] = static_cast<char>(n >> (8 * i));
}
}
std::string clipboardDibBitmap(std::string_view dib) {
    if (dib.size() > maxClipboardImageBytes) throw std::runtime_error("Clipboard image exceeds the 64 MiB limit.");
    const auto header = value(dib, 0);
    if ((header != 40 && header != 52 && header != 56 && header != 108 && header != 124) || header > dib.size())
        throw std::runtime_error("Unsupported clipboard bitmap header.");
    const auto width = value(dib, 4), rawHeight = value(dib, 8);
    const uint64_t height = rawHeight & 0x80000000u ? 0x100000000ull - rawHeight : rawHeight;
    const auto bits = value(dib, 14, 2), compression = value(dib, 16), colors = value(dib, 32);
    if (!width || width > 16384 || !height || height > 16384 || value(dib, 12, 2) != 1 ||
        (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        (compression != 0 && compression != 3 && compression != 6) ||
        (compression && bits != 16 && bits != 32) || colors > 256 || (bits <= 8 && colors > (1u << bits)))
        throw std::runtime_error("Unsupported clipboard bitmap dimensions or pixel format.");
    uint64_t offset = header + (header == 40 && compression ? (compression == 6 ? 16 : 12) : 0);
    offset += 4ull * (colors ? colors : bits <= 8 ? 1u << bits : 0);
    if (header == 124) {
        const auto profile = value(dib, 112), size = value(dib, 116);
        if (size && (profile < offset || static_cast<uint64_t>(profile) + size > dib.size()))
            throw std::runtime_error("Invalid clipboard color profile.");
        if (size && profile == offset) offset += size;
    }
    const uint64_t pixels = ((static_cast<uint64_t>(width) * bits + 31) / 32) * 4 * height;
    if (static_cast<uint64_t>(width) * height * 4 > maxClipboardImageBytes || offset + pixels > dib.size())
        throw std::runtime_error("Clipboard bitmap pixels are truncated or exceed the 64 MiB limit.");
    std::string bitmap(14, '\0'); bitmap[0] = 'B'; bitmap[1] = 'M';
    put(bitmap, 2, static_cast<uint32_t>(dib.size() + 14)); put(bitmap, 10, static_cast<uint32_t>(offset + 14));
    bitmap.append(dib); return bitmap;
}
std::wstring pastedImageName(const std::tm& time) {
    wchar_t name[64]{};
    std::swprintf(name, 64, L"paste-%04d-%02d-%02dT%02d-%02d-%02d.png",
        time.tm_year + 1900, time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);
    return name;
}
}
