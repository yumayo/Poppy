#include "taskbar_badge.h"
#include "render.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace poppy {
namespace {
// Builds the icon from an in-memory icon resource: user32 only, no GDI bitmap.
HICON iconFromPixels(const std::vector<BYTE>& pixels, int size) {
    const UINT stride = static_cast<UINT>(size) * 4, maskStride = (static_cast<UINT>(size) + 31) / 32 * 4;
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header); header.biWidth = size; header.biHeight = size * 2; // Color rows, then the AND mask.
    header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB;
    std::vector<BYTE> resource(sizeof(header) + pixels.size() + maskStride * size);
    std::memcpy(resource.data(), &header, sizeof(header));
    BYTE* color = resource.data() + sizeof(header);
    for (int y = 0; y < size; ++y) {
        // Icon rows are bottom-up with straight alpha.
        const BYTE* from = pixels.data() + static_cast<size_t>(size - 1 - y) * stride;
        BYTE* to = color + static_cast<size_t>(y) * stride;
        for (int x = 0; x < size; ++x, from += 4, to += 4) {
            const BYTE alpha = from[3];
            for (int c = 0; c < 3; ++c) to[c] = alpha ? static_cast<BYTE>(std::min(255, from[c] * 255 / alpha)) : 0;
            to[3] = alpha;
        }
    }
    // The zeroed AND mask leaves transparency to the alpha channel.
    return CreateIconFromResourceEx(resource.data(), static_cast<DWORD>(resource.size()), TRUE, 0x00030000, size, size, LR_DEFAULTCOLOR);
}
HICON badgedIcon(HINSTANCE instance, int iconId, int size, size_t count) {
    HICON original = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(iconId), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (!original) return nullptr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source, bitmap;
    ComPtr<IWICFormatConverter> converted;
    std::vector<BYTE> pixels(static_cast<size_t>(size) * size * 4);
    const bool drawn =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put()))) &&
        SUCCEEDED(factory->CreateBitmapFromHICON(original, source.put())) &&
        SUCCEEDED(factory->CreateFormatConverter(converted.put())) &&
        SUCCEEDED(converted->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(factory->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.put())) &&
        drawBadgedIcon(bitmap.get(), converted.get(), count) &&
        SUCCEEDED(bitmap->CopyPixels(nullptr, static_cast<UINT>(size) * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    DestroyIcon(original);
    return drawn ? iconFromPixels(pixels, size) : nullptr;
}
}
TaskbarBadge::~TaskbarBadge() { release(); }
void TaskbarBadge::release() {
    if (big_) DestroyIcon(big_);
    if (small_) DestroyIcon(small_);
    big_ = small_ = nullptr;
}
void TaskbarBadge::attach(HWND window, HINSTANCE instance, int iconId) { window_ = window; instance_ = instance; iconId_ = iconId; }
void TaskbarBadge::update(size_t count, UINT dpi) {
    count_ = count; dpi_ = dpi;
    if (window_ && (shown_ != count_ || (count_ && shownDpi_ != dpi_))) apply();
}
void TaskbarBadge::apply() {
    HICON big = nullptr, small = nullptr;
    if (count_) {
        big = badgedIcon(instance_, iconId_, GetSystemMetricsForDpi(SM_CXICON, dpi_), count_);
        small = badgedIcon(instance_, iconId_, GetSystemMetricsForDpi(SM_CXSMICON, dpi_), count_);
        if (!big || !small) { if (big) DestroyIcon(big); if (small) DestroyIcon(small); return; }
    }
    // Null window icons fall back to the class icons registered with the window.
    SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    release();
    big_ = big; small_ = small; shown_ = count_; shownDpi_ = dpi_;
}
}
