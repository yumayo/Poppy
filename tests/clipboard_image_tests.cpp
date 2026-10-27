#include "clipboard_image.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include "render.h"
#endif

using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put(std::string& bytes, size_t at, uint32_t n, size_t count = 4) {
    for (size_t i = 0; i < count; ++i) bytes[at + i] = static_cast<char>(n >> (8 * i));
}
std::string dib(uint32_t header = 40, unsigned bits = 24, uint32_t compression = 0, int height = 1) {
    const size_t masks = header == 40 && compression ? (compression == 6 ? 16 : 12) : 0;
    const size_t palette = bits <= 8 ? 4 * (1u << bits) : 0;
    std::string bytes(header + masks + palette + 4, '\0');
    put(bytes, 0, header); put(bytes, 4, 1); put(bytes, 8, static_cast<uint32_t>(height));
    put(bytes, 12, 1, 2); put(bytes, 14, bits, 2); put(bytes, 16, compression);
    return bytes;
}
void invalid(const std::string& bytes) {
    bool rejected = false;
    try { clipboardDibBitmap(bytes); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "Malformed bitmap was accepted");
}
void portableTests() {
    std::tm time{}; time.tm_year = 126; time.tm_mon = 9; time.tm_mday = 27; time.tm_hour = 8; time.tm_min = 3; time.tm_sec = 9;
    check(pastedImageName(time) == L"paste-2026-10-27T08-03-09.png", "Paste filename has incorrect timestamp fields");
    for (const auto header : {40u, 52u, 56u, 108u, 124u}) for (const auto bits : {1u, 4u, 8u, 16u, 24u, 32u}) {
        const auto source = dib(header, bits), bitmap = clipboardDibBitmap(source);
        check(bitmap.substr(0, 2) == "BM" && bitmap.substr(14) == source, "BMP wrapping changed the bitmap bytes");
        const auto offset = static_cast<unsigned char>(bitmap[10]) + (static_cast<unsigned char>(bitmap[11]) << 8);
        check(static_cast<size_t>(offset) == bitmap.size() - 4, "Palette or header offset points outside pixel data");
    }
    check(clipboardDibBitmap(dib(40, 24, 0, -1)).size() == 58, "Top-down DIB was rejected");
    for (const auto compression : {3u, 6u}) {
        const auto bitmap = clipboardDibBitmap(dib(40, 32, compression));
        check(static_cast<unsigned char>(bitmap[10]) == 54 + (compression == 3 ? 12 : 16), "External bit masks were counted as pixels");
    }
    auto profile = dib(124); profile.insert(124, 4, 'p'); put(profile, 112, 124); put(profile, 116, 4);
    check(static_cast<unsigned char>(clipboardDibBitmap(profile)[10]) == 142, "V5 profile was counted as pixels");
    invalid(""); invalid(std::string(39, '\0'));
    auto truncated = dib(); truncated.pop_back(); invalid(truncated);
    auto bad = dib(); put(bad, 4, 0xffffffffu); invalid(bad);
    bad = dib(); put(bad, 8, 0x80000000u); invalid(bad);
    bad = dib(); put(bad, 16, 1); invalid(bad);
    bad = dib(); put(bad, 32, 257); invalid(bad);
    bad = dib(124); put(bad, 112, 124); put(bad, 116, 1000); invalid(bad);
    invalid(std::string(maxClipboardImageBytes + 1, '\0'));
}
#ifdef _WIN32
void windowsTests() {
    check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "Cannot initialize WIC test");
    struct Uninitialize { ~Uninitialize() { CoUninitialize(); } } uninitialize;
    auto source = dib(); source[42] = static_cast<char>(255); // One red BGR pixel, with row padding.
    const auto png = encodeClipboardPng({source, true});
    check(png.substr(0, 8) == std::string("\x89PNG\r\n\x1a\n", 8), "Image was not encoded as PNG");
    check(encodeClipboardPng({png, false}).substr(0, 8) == png.substr(0, 8), "Registered PNG clipboard format failed");
    auto alpha = dib(124, 32, 3, -1);
    put(alpha, 40, 0xff0000); put(alpha, 44, 0xff00); put(alpha, 48, 0xff); put(alpha, 52, 0xff000000);
    alpha[126] = static_cast<char>(255); alpha[127] = static_cast<char>(128);
    auto encoded = encodeClipboardPng({alpha, true});
    ComPtr<IWICImagingFactory> factory;
    check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put()))), "Cannot create test decoder");
    ComPtr<IWICStream> stream; check(SUCCEEDED(factory->CreateStream(stream.put())), "Cannot create test stream");
    check(SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size()))), "Cannot read encoded image");
    ComPtr<IWICBitmapDecoder> decoder; check(SUCCEEDED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())), "PNG cannot be decoded");
    ComPtr<IWICBitmapFrameDecode> frame; check(SUCCEEDED(decoder->GetFrame(0, frame.put())), "PNG has no frame");
    ComPtr<IWICFormatConverter> converter; check(SUCCEEDED(factory->CreateFormatConverter(converter.put())), "Cannot convert test pixels");
    check(SUCCEEDED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "Cannot decode PNG pixels");
    BYTE pixel[4]{}; check(SUCCEEDED(converter->CopyPixels(nullptr, 4, 4, pixel)), "Cannot read PNG pixels");
    check(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 128, "PNG changed the pixel color or alpha");
}
#endif
}
int main() {
    try { portableTests();
#ifdef _WIN32
        windowsTests();
#endif
        std::cout << "Clipboard image tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
