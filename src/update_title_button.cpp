#include "update_title_button.h"
#include <dwmapi.h>
#include <stdexcept>

namespace poppy {
void UpdateTitleButton::create(HWND owner, HINSTANCE instance) {
    owner_ = owner;
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance;
    cls.lpszClassName = L"PoppyUpdateTitleButton"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    // An owned, nonactivating surface can occupy the native caption without
    // replacing Windows' title bar, resize borders or caption buttons.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName,
        L"Restart to update Poppy", WS_POPUP, 0, 0, 0, 0, owner, nullptr, instance, this);
    if (!window_) throw std::runtime_error("Cannot create the update button");
}
void UpdateTitleButton::available(bool value) {
    if (available_ == value) return;
    available_ = value;
    const auto menu = GetSystemMenu(owner_, FALSE);
    if (value) AppendMenuW(menu, MF_STRING, restartUpdateCommand, L"Restart to update Poppy");
    else DeleteMenu(menu, restartUpdateCommand, MF_BYCOMMAND);
    cancel(); layout();
}
void UpdateTitleButton::layout() {
    if (!window_) return;
    RECT frame{}, buttons{};
    if (!available_ || !IsWindowVisible(owner_) || IsIconic(owner_) || !IsWindowEnabled(owner_) ||
        !GetWindowRect(owner_, &frame) ||
        FAILED(DwmGetWindowAttribute(owner_, DWMWA_CAPTION_BUTTON_BOUNDS, &buttons, sizeof(buttons))) ||
        buttons.right <= buttons.left || buttons.bottom <= buttons.top) {
        ShowWindow(window_, SW_HIDE); return;
    }
    const auto dpi = GetDpiForWindow(owner_);
    if (dpi_ != dpi) { dpi_ = dpi; renderer_.setDpi(dpi); }
    // DWM supplies the actual minimize-button edge, including maximized/DPI layouts.
    const int width = MulDiv(80, dpi_, 96), margin = MulDiv(4, dpi_, 96);
    SetWindowPos(window_, nullptr, frame.left + buttons.left - width, frame.top + buttons.top + margin,
        width, buttons.bottom - buttons.top - 2 * margin, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}
bool UpdateTitleButton::contains(LPARAM point) const {
    RECT rect{}; GetClientRect(window_, &rect);
    return PtInRect(&rect, {GET_X_LPARAM(point), GET_Y_LPARAM(point)}) != FALSE;
}
void UpdateTitleButton::cancel() {
    pressed_ = hover_ = false;
    if (GetCapture() == window_) ReleaseCapture();
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}
void UpdateTitleButton::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    RECT rect{}; GetClientRect(window_, &rect);
    renderer_.fill(rect, accent);
    InflateRect(&rect, -1, -1);
    renderer_.fill(rect, pressed_ && hover_ ? border : hover_ ? panel : RGB(32, 32, 32));
    renderer_.label(rect, L"Restart", accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
LRESULT CALLBACK UpdateTitleButton::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto self = reinterpret_cast<UpdateTitleButton*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<UpdateTitleButton*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(message, a, b) : DefWindowProcW(window, message, a, b);
}
LRESULT UpdateTitleButton::handle(UINT message, WPARAM a, LPARAM b) {
    switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        const bool inside = contains(b);
        if (!hover_ && inside) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track); }
        if (hover_ != inside) { hover_ = inside; InvalidateRect(window_, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE: hover_ = false; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: pressed_ = true; hover_ = contains(b); SetCapture(window_); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: {
        const bool click = pressed_ && contains(b); cancel();
        if (click && available_) PostMessageW(owner_, WM_SYSCOMMAND, restartUpdateCommand, 0);
        return 0;
    }
    case BM_CLICK: if (available_) PostMessageW(owner_, WM_SYSCOMMAND, restartUpdateCommand, 0); return 0;
    case WM_CANCELMODE: case WM_CAPTURECHANGED: cancel(); return 0;
    case WM_DESTROY: cancel(); renderer_.discard(); return 0;
    case WM_NCDESTROY: {
        const auto window = window_; window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, a, b);
    }
    }
    return DefWindowProcW(window_, message, a, b);
}
}
