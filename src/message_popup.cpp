#include "message_popup.h"
#include <algorithm>
#include <commctrl.h>

namespace poppy {
namespace { constexpr UINT_PTR leaveTimer = 1; }
void MessagePopup::show(HWND owner, const std::wstring& message, const RECT& anchor, UINT dpi) {
    if (message.empty()) return;
    if (!window_) {
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance;
        cls.lpszClassName = L"PoppyMessagePopup"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        owner_ = owner;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Commit message", WS_POPUP | WS_CLIPCHILDREN,
            0, 0, 0, 0, owner, nullptr, instance, this);
        if (!window_) return;
        body_.create(window_, instance, true);
        SetWindowSubclass(body_.window(), bodyProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    dpi_ = dpi; renderer_.setDpi(dpi); body_.setDpi(dpi);
    message_ = message; anchor_ = anchor; outsideSince_ = 0;
    copyHot_ = copyPressed_ = false; previousFocus_ = GetFocus();
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto& work = monitor.rcWork;
    const int width = std::min(px(560), static_cast<int>(work.right - work.left));
    const int height = std::min(px(320), static_cast<int>(work.bottom - work.top));
    int x = anchor.right - px(8);
    if (x + width > work.right) x = anchor.left - width + px(8);
    x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
    const int y = std::clamp(static_cast<int>(anchor.top), static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    // Size the body before building wrapped rows, including on first use.
    SetWindowPos(window_, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
    auto document = std::make_shared<Document>(); document->text = utf8(message); indexDocument(*document);
    body_.show(std::move(document), {}, false, false);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    SetTimer(window_, leaveTimer, 80, nullptr);
    InvalidateRect(window_, nullptr, FALSE);
}
void MessagePopup::hide(bool restoreFocus) {
    if (!window_) return;
    KillTimer(window_, leaveTimer);
    if (contains(GetCapture())) ReleaseCapture();
    const bool restore = restoreFocus && contains(GetFocus());
    ShowWindow(window_, SW_HIDE);
    if (restore && IsWindow(previousFocus_) && IsWindowVisible(previousFocus_)) SetFocus(previousFocus_);
    copyHot_ = copyPressed_ = false;
    // Do not retain a second document after its history row disappears.
    body_.message(L""); message_.clear();
}
void MessagePopup::destroy() { hide(); if (window_) DestroyWindow(window_); }
RECT MessagePopup::copyBounds() const {
    RECT r{}; GetClientRect(window_, &r);
    return {std::max(0L, r.right - px(100)), px(4), r.right - px(8), px(32)};
}
void MessagePopup::tick() {
    const auto root = GetAncestor(owner_, GA_ROOT);
    if (!IsWindowVisible(owner_) || IsIconic(root)) { hide(); return; }
    POINT point{}; GetCursorPos(&point);
    RECT bounds{}; GetWindowRect(window_, &bounds);
    // Capture keeps drag selection alive beyond the popup. Context menus run a
    // nested message loop and can extend beyond either hover rectangle.
    const auto under = WindowFromPoint(point);
    if (menuOpen_ || contains(GetCapture()) ||
        (PtInRect(&bounds, point) && contains(under)) || (PtInRect(&anchor_, point) && under == owner_)) {
        outsideSince_ = 0; return;
    }
    const auto now = GetTickCount64();
    if (!outsideSince_) outsideSince_ = now;
    else if (now - outsideSince_ >= 300) hide();
}
LRESULT CALLBACK MessagePopup::bodyProcedure(HWND window, UINT m, WPARAM a, LPARAM b, UINT_PTR id, DWORD_PTR data) {
    auto& self = *reinterpret_cast<MessagePopup*>(data);
    if (m == WM_KEYDOWN && a == VK_ESCAPE) { self.hide(); return 0; }
    if (m == WM_CONTEXTMENU) {
        self.menuOpen_ = true;
        const auto result = DefSubclassProc(window, m, a, b);
        self.menuOpen_ = false; self.outsideSince_ = 0;
        return result;
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(window, bodyProcedure, id);
    return DefSubclassProc(window, m, a, b);
}
LRESULT CALLBACK MessagePopup::procedure(HWND window, UINT m, WPARAM a, LPARAM b) {
    auto self = reinterpret_cast<MessagePopup*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<MessagePopup*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, m, a, b);
    const auto result = self->handle(m, a, b);
    if (m == WM_NCDESTROY) { SetWindowLongPtrW(window, GWLP_USERDATA, 0); self->window_ = nullptr; }
    return result;
}
LRESULT MessagePopup::handle(UINT m, WPARAM a, LPARAM b) {
    switch (m) {
    case WM_PAINT: {
        Renderer::Paint paint(renderer_, window_); if (!paint) return 0;
        RECT r{}; GetClientRect(window_, &r); renderer_.fill(r, panel); renderer_.outline(r, border);
        const auto copy = copyBounds();
        renderer_.label({px(12), 0, copy.left - px(8), px(36)}, L"Commit message", foreground);
        if (copyHot_) renderer_.fill(copy, RGB(255, 255, 255), 0.1f);
        renderer_.label(copy, L"Copy all", accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: {
        RECT r{}; GetClientRect(window_, &r);
        SetWindowPos(body_.window(), nullptr, px(1), px(36), std::max(0L, r.right - px(2)),
            std::max(0L, r.bottom - px(37)), SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TIMER: if (a == leaveTimer) tick(); return 0;
    case WM_ACTIVATEAPP: if (!a) hide(false); return 0;
    case WM_CLOSE: hide(); return 0;
    case WM_KEYDOWN: if (a == VK_ESCAPE) hide(); return 0;
    case WM_MOUSEMOVE: {
        const auto copy = copyBounds(); const bool hot = PtInRect(&copy, {GET_X_LPARAM(b), GET_Y_LPARAM(b)}) != FALSE;
        if (hot != copyHot_) { copyHot_ = hot; InvalidateRect(window_, nullptr, FALSE); }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track); return 0;
    }
    case WM_MOUSELEAVE: copyHot_ = false; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        const auto copy = copyBounds(); copyPressed_ = PtInRect(&copy, {GET_X_LPARAM(b), GET_Y_LPARAM(b)}) != FALSE;
        if (copyPressed_) SetCapture(window_);
        return 0;
    }
    case WM_LBUTTONUP: {
        const bool pressed = std::exchange(copyPressed_, false);
        if (GetCapture() == window_) ReleaseCapture();
        const auto copy = copyBounds();
        if (pressed && PtInRect(&copy, {GET_X_LPARAM(b), GET_Y_LPARAM(b)})) copyText(window_, message_);
        return 0;
    }
    case WM_CAPTURECHANGED: case WM_CANCELMODE: copyPressed_ = false; return 0;
    case WM_DESTROY: KillTimer(window_, leaveTimer); renderer_.discard(); return 0;
    default: return DefWindowProcW(window_, m, a, b);
    }
}
}
