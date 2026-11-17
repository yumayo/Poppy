#include "taskbar_badge.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace poppy {
namespace {
// Builds the icon from an in-memory icon resource: user32 only, no GDI bitmap.
HICON createBadgeIcon(size_t count, int size) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put())))) return nullptr;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.put())) || !drawCountBadge(bitmap.get(), count)) return nullptr;
    const UINT stride = static_cast<UINT>(size) * 4;
    std::vector<BYTE> pixels(stride * size);
    if (FAILED(bitmap->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) return nullptr;
    const UINT maskStride = (static_cast<UINT>(size) + 31) / 32 * 4;
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
}
TaskbarBadge::~TaskbarBadge() { if (icon_) DestroyIcon(icon_); }
void TaskbarBadge::reset(HWND window) {
    window_ = window; taskbar_.reset(); shown_ = 0; shownDpi_ = 0;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, reinterpret_cast<void**>(taskbar_.put()))) || FAILED(taskbar_->HrInit())) { taskbar_.reset(); return; }
    apply();
}
void TaskbarBadge::update(size_t count, UINT dpi) {
    count_ = count; dpi_ = dpi;
    if (taskbar_ && (shown_ != count_ || (count_ && shownDpi_ != dpi_))) apply();
}
void TaskbarBadge::apply() {
    HICON icon = nullptr;
    std::wstring description;
    if (count_) {
        icon = createBadgeIcon(count_, GetSystemMetricsForDpi(SM_CXSMICON, dpi_));
        if (!icon) return;
        description = std::to_wstring(count_) + (count_ == 1 ? L" unread AI session" : L" unread AI sessions");
    }
    if (FAILED(taskbar_->SetOverlayIcon(window_, icon, count_ ? description.c_str() : nullptr))) { if (icon) DestroyIcon(icon); return; }
    // The taskbar keeps its own copy of the overlay.
    if (icon_) DestroyIcon(icon_);
    icon_ = icon; shown_ = count_; shownDpi_ = dpi_;
}
}
