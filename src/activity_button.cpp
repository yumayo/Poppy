#include "activity_button.h"
#include <algorithm>

namespace poppy {
void ActivityButton::create(HWND parent, HINSTANCE instance, const wchar_t* title, int id, bool git, bool branch) {
    git_ = git; branch_ = branch; dpi_ = GetDpiForWindow(parent);
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = activityButtonClassName;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    window_ = CreateWindowExW(0, activityButtonClassName, title, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
}
bool ActivityButton::contains(LPARAM point) const {
    RECT rect{}; GetClientRect(window_, &rect);
    return PtInRect(&rect, {GET_X_LPARAM(point), GET_Y_LPARAM(point)}) != FALSE;
}
void ActivityButton::activate() { if (IsWindowEnabled(window_) && clicked) clicked(); }
void ActivityButton::cancel() {
    mousePressed_ = keyPressed_ = false;
    if (GetCapture() == window_) ReleaseCapture();
    InvalidateRect(window_, nullptr, FALSE);
}
void ActivityButton::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    RECT rect{}; GetClientRect(window_, &rect);
    const bool enabled = IsWindowEnabled(window_) != FALSE;
    renderer_.fill(rect, selected_ || (mousePressed_ && hover_) || keyPressed_ ? panel : hover_ && enabled ? RGB(34, 39, 47) : background);
    const COLORREF color = !enabled ? border : selected_ ? foreground : muted;
    const int size = MulDiv(24, dpi_, 96), x = (rect.right - size) / 2, y = (rect.bottom - size) / 2;
    renderer_.icon(branch_ ? Renderer::Icon::Branch : git_ ? Renderer::Icon::SourceControl : Renderer::Icon::Explorer,
        {x, y, x + size, y + size}, color);
    if (selected_) renderer_.fill({0, MulDiv(8, dpi_, 96), MulDiv(3, dpi_, 96), rect.bottom - MulDiv(8, dpi_, 96)}, accent);
    if (enabled && GetFocus() == window_) { RECT focus = rect; InflateRect(&focus, -4, -4); renderer_.outline(focus, muted); }
}
LRESULT CALLBACK ActivityButton::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<ActivityButton*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<ActivityButton*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT ActivityButton::handle(UINT m, WPARAM a, LPARAM b) {
    switch (m) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: case WM_SETFOCUS: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_BUTTON;
    case WM_ENABLE: case WM_KILLFOCUS: case WM_CANCELMODE: cancel(); return 0;
    case WM_MOUSEMOVE: {
        const bool inside = contains(b);
        if (!hover_ && inside) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track); }
        if (hover_ != inside) { hover_ = inside; InvalidateRect(window_, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE: hover_ = false; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        if (IsWindowEnabled(window_)) { SetFocus(window_); mousePressed_ = true; hover_ = contains(b); SetCapture(window_); InvalidateRect(window_, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        const bool click = mousePressed_ && contains(b); cancel(); if (click) activate(); return 0;
    }
    case WM_CAPTURECHANGED: mousePressed_ = false; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_KEYDOWN:
        if (!IsWindowEnabled(window_)) return 0;
        if (a == VK_SPACE) { keyPressed_ = true; InvalidateRect(window_, nullptr, FALSE); }
        else if (a == VK_RETURN && !(b & (static_cast<LPARAM>(1) << 30))) activate();
        return 0;
    case WM_KEYUP: if (a == VK_SPACE && keyPressed_) { keyPressed_ = false; InvalidateRect(window_, nullptr, FALSE); activate(); } return 0;
    case WM_CHAR: return 0;
    case BM_CLICK: activate(); return 0;
    case WM_DESTROY: cancel(); renderer_.discard(); return 0;
    default: return DefWindowProcW(window_, m, a, b);
    }
}
}
