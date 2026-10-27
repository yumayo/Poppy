#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace poppy {
constexpr size_t maxImagePixelBytes = 64 * 1024 * 1024;
struct ImagePreview {
    uint32_t width = 0, height = 0;
    // Top-down, premultiplied BGRA. No COM objects cross the I/O/UI boundary.
    std::vector<uint8_t> pixels;
};
bool isImageExtension(std::wstring_view extension);
size_t imagePixelBytes(uint32_t width, uint32_t height);
struct ImagePlacement { int x = 0, y = 0, width = 0, height = 0; };
ImagePlacement fitImage(uint32_t width, uint32_t height, int availableWidth, int availableHeight);
#ifdef _WIN32
// Decode the first frame on the existing file worker, before publishing pixels.
std::shared_ptr<const ImagePreview> decodeImage(std::string_view bytes);
#endif
}
