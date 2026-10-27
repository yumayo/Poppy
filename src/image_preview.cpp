#include "image_preview.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace poppy {
bool isImageExtension(std::wstring_view extension) {
    std::wstring lower(extension);
    for (auto& c : lower) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return lower == L".png" || lower == L".jpg" || lower == L".jpeg" || lower == L".bmp" ||
        lower == L".gif" || lower == L".tif" || lower == L".tiff" || lower == L".ico";
}
size_t imagePixelBytes(uint32_t width, uint32_t height) {
    if (!width || !height || width > 16384 || height > 16384 || static_cast<uint64_t>(width) * height * 4 > maxImagePixelBytes)
        throw std::runtime_error("Image exceeds the 16,384-pixel dimension or 64 MiB pixel limit.");
    return static_cast<size_t>(width) * height * 4;
}
ImagePlacement fitImage(uint32_t width, uint32_t height, int availableWidth, int availableHeight) {
    if (!width || !height || availableWidth <= 0 || availableHeight <= 0) return {};
    const double scale = std::min({1.0, static_cast<double>(availableWidth) / width, static_cast<double>(availableHeight) / height});
    const int w = std::max(1, static_cast<int>(width * scale)), h = std::max(1, static_cast<int>(height * scale));
    return {(availableWidth - w) / 2, (availableHeight - h) / 2, w, h};
}
}
