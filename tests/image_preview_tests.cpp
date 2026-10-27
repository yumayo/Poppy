#include "image_preview.h"
#include <iostream>
#include <stdexcept>
#include <string>

using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "Invalid or oversized image was accepted");
}
void portableTests() {
    for (const auto extension : {L".png", L".PNG", L".JpEg", L".jpg", L".bmp", L".gif", L".tif", L".tiff", L".ico"})
        check(isImageExtension(extension), "A supported image extension was not recognized");
    for (const auto extension : {L"", L".cpp", L".svg", L".png.txt", L"png"})
        check(!isImageExtension(extension), "A non-image was routed to the image decoder");
    check(imagePixelBytes(4096, 4096) == maxImagePixelBytes, "The pixel memory boundary was rejected");
    check(imagePixelBytes(16384, 1) == 65536, "A narrow image was rejected");
    rejects([] { imagePixelBytes(0, 1); }); rejects([] { imagePixelBytes(1, 0); });
    rejects([] { imagePixelBytes(16385, 1); }); rejects([] { imagePixelBytes(4096, 4097); });
    rejects([] { imagePixelBytes(UINT32_MAX, UINT32_MAX); });
    auto fit = fitImage(800, 400, 300, 300);
    check(fit.x == 0 && fit.y == 75 && fit.width == 300 && fit.height == 150, "Landscape image was stretched or misplaced");
    fit = fitImage(400, 800, 300, 300);
    check(fit.x == 75 && fit.y == 0 && fit.width == 150 && fit.height == 300, "Portrait image was stretched or misplaced");
    fit = fitImage(20, 10, 300, 300);
    check(fit.x == 140 && fit.y == 145 && fit.width == 20 && fit.height == 10, "Small image was enlarged");
    check(fitImage(100, 100, 0, 30).width == 0 && fitImage(100, 100, 30, -1).height == 0, "An empty viewport produced an image");
    fit = fitImage(16384, 1, 1, 1);
    check(fit.width == 1 && fit.height == 1, "A very thin image disappeared");
}
#ifdef _WIN32
void put(std::string& bytes, size_t at, uint32_t value, size_t count = 4) {
    for (size_t i = 0; i < count; ++i) bytes[at + i] = static_cast<char>(value >> (8 * i));
}
void windowsTests() {
    // A bottom-up BMP with a blue bottom row and a red top row.
    std::string bmp(62, '\0'); bmp[0] = 'B'; bmp[1] = 'M';
    put(bmp, 2, 62); put(bmp, 10, 54); put(bmp, 14, 40); put(bmp, 18, 1); put(bmp, 22, 2);
    put(bmp, 26, 1, 2); put(bmp, 28, 24, 2); bmp[54] = static_cast<char>(255); bmp[60] = static_cast<char>(255);
    const auto image = decodeImage(bmp);
    check(image->width == 1 && image->height == 2 && image->pixels.size() == 8, "BMP dimensions were lost");
    check(image->pixels == std::vector<uint8_t>({0, 0, 255, 255, 255, 0, 0, 255}), "BMP colors or row direction changed");
    // One half-transparent red RGBA pixel, exercising PNG and premultiplication.
    const unsigned char png[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,
        0x00,0x00,0x00,0x0d,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xcf,0xc0,0xd0,0x00,0x00,
        0x04,0x81,0x01,0x80,0x2c,0x55,0xce,0xb0,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
    const auto alpha = decodeImage(std::string_view(reinterpret_cast<const char*>(png), sizeof(png)));
    check(alpha->width == 1 && alpha->height == 1 && alpha->pixels == std::vector<uint8_t>({0, 0, 128, 128}), "PNG transparency was not premultiplied");
    rejects([] { decodeImage(""); }); rejects([] { decodeImage("not an image"); });
    rejects([&] { decodeImage(bmp.substr(0, 30)); });
    put(bmp, 18, 16385); rejects([&] { decodeImage(bmp); });
    rejects([] { decodeImage(std::string(16 * 1024 * 1024 + 1, '\0')); });
}
#endif
}
int main() {
    try {
        portableTests();
#ifdef _WIN32
        windowsTests();
#endif
        std::cout << "Image preview tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
