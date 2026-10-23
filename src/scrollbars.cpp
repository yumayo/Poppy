#include "scrollbars.h"
#include <algorithm>
#include <cstdint>

namespace poppy {
RECT Scrollbars::contentRect(HWND window) {
    RECT rect{}; GetClientRect(window, &rect);
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const UINT dpi = GetDpiForWindow(window);
    if (style & WS_VSCROLL) rect.right = std::max(0L, rect.right - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi));
    if (style & WS_HSCROLL) rect.bottom = std::max(0L, rect.bottom - GetSystemMetricsForDpi(SM_CYHSCROLL, dpi));
    return rect;
}
Scrollbars::Geometry Scrollbars::geometry(HWND window, int bar) {
    Geometry g;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!(style & (bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL))) return g;
    RECT client{}; GetClientRect(window, &client);
    const auto content = contentRect(window);
    g.bounds = bar == SB_VERT ? RECT{content.right, 0, client.right, content.bottom}
                              : RECT{0, content.bottom, content.right, client.bottom};
    const int length = bar == SB_VERT ? g.bounds.bottom : g.bounds.right;
    const int width = bar == SB_VERT ? g.bounds.right - g.bounds.left : g.bounds.bottom - g.bounds.top;
    const int arrow = std::min(width, length / 2);
    g.start = arrow; g.end = length - arrow;
    GetScrollInfo(window, bar, &g.info);
    const auto range = static_cast<int64_t>(g.info.nMax) - g.info.nMin + 1;
    g.maximum = static_cast<int>(std::max<int64_t>(g.info.nMin, static_cast<int64_t>(g.info.nMax) - std::max(1u, g.info.nPage) + 1));
    const int track = g.end - g.start;
    const int minimum = std::min(track, MulDiv(20, GetDpiForWindow(window), 96));
    const int thumb = range > 0 ? static_cast<int>(std::clamp<int64_t>(track * static_cast<int64_t>(g.info.nPage) / range, minimum, track)) : track;
    const auto span = static_cast<int64_t>(g.maximum) - g.info.nMin;
    g.thumbStart = g.start + (span > 0 ? static_cast<int>((track - thumb) * (static_cast<int64_t>(g.info.nPos) - g.info.nMin) / span) : 0);
    g.thumbEnd = g.thumbStart + thumb;
    return g;
}
Scrollbars::Hit Scrollbars::hit(HWND window, POINT point) {
    for (const int bar : {SB_VERT, SB_HORZ}) {
        const auto g = geometry(window, bar);
        if (!PtInRect(&g.bounds, point)) continue;
        const int p = bar == SB_VERT ? point.y : point.x;
        return {bar, p < g.start ? Back : p >= g.end ? Forward : p < g.thumbStart ? PageBack : p >= g.thumbEnd ? PageForward : Thumb};
    }
    RECT client{}; GetClientRect(window, &client);
    if (PtInRect(&client, point)) {
        const auto content = contentRect(window);
        if (point.x >= content.right && point.y >= content.bottom) return {SB_VERT, Corner};
    }
    return {};
}
void Scrollbars::paint(HWND window, Renderer& renderer, COLORREF surface) {
    RECT client{}; GetClientRect(window, &client);
    const auto content = contentRect(window);
    renderer.fill({content.right, content.bottom, client.right, client.bottom}, surface);
    for (const int bar : {SB_VERT, SB_HORZ}) {
        const auto g = geometry(window, bar);
        if (IsRectEmpty(&g.bounds)) continue;
        renderer.fill(g.bounds, surface);
        const bool enabled = g.maximum > g.info.nMin;
        const int padding = std::max(1, MulDiv(3, GetDpiForWindow(window), 96));
        if (enabled) {
            const COLORREF color = pressed_.bar == bar && pressed_.part == Thumb ? muted
                : hot_.bar == bar && hot_.part == Thumb ? scrollbarHover : scrollbarThumb;
            const RECT thumb = bar == SB_VERT ? RECT{g.bounds.left + padding, g.thumbStart, g.bounds.right - padding, g.thumbEnd}
                : RECT{g.thumbStart, g.bounds.top + padding, g.thumbEnd, g.bounds.bottom - padding};
            renderer.fill(thumb, color);
        }
        for (const Part part : {Back, Forward}) {
            RECT button = g.bounds;
            if (bar == SB_VERT) { if (part == Back) button.bottom = g.start; else button.top = g.end; }
            else { if (part == Back) button.right = g.start; else button.left = g.end; }
            if (enabled && hot_.bar == bar && hot_.part == part) renderer.fill(button, border);
            const float x = (button.left + button.right) / 2.0f, y = (button.top + button.bottom) / 2.0f;
            const float size = std::min(button.right - button.left, button.bottom - button.top) / 5.0f;
            const float direction = part == Back ? -1.0f : 1.0f;
            const COLORREF color = !enabled ? border : pressed_.bar == bar && pressed_.part == part ? foreground : muted;
            const float stroke = static_cast<float>(std::max(1, MulDiv(1, GetDpiForWindow(window), 96)));
            if (bar == SB_VERT) {
                renderer.line(x - size, y - direction * size / 2, x, y + direction * size / 2, color, stroke);
                renderer.line(x, y + direction * size / 2, x + size, y - direction * size / 2, color, stroke);
            } else {
                renderer.line(x - direction * size / 2, y - size, x + direction * size / 2, y, color, stroke);
                renderer.line(x + direction * size / 2, y, x - direction * size / 2, y + size, color, stroke);
            }
        }
    }
}
int Scrollbars::trackPosition(int bar, int fallback) const {
    return pressed_.part == Thumb && pressed_.bar == bar ? trackPosition_ : fallback;
}
void Scrollbars::notify(HWND window, UINT code) {
    SendMessageW(window, pressed_.bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, MAKEWPARAM(code, 0), 0);
}
void Scrollbars::cancel(HWND window) {
    KillTimer(window, repeatTimer); pressed_ = {}; hot_ = {};
    if (GetCapture() == window) ReleaseCapture();
    InvalidateRect(window, nullptr, FALSE);
}
bool Scrollbars::handle(HWND window, UINT message, WPARAM a, LPARAM b, LRESULT& result) {
    result = 0;
    switch (message) {
    // These borderless panes use the full client area. Suppress the native
    // nonclient bars while retaining SetScrollInfo/GetScrollInfo and WS_*SCROLL.
    case WM_NCCALCSIZE: case WM_NCPAINT: return true;
    case WM_NCACTIVATE: result = TRUE; return true;
    case WM_SETCURSOR: {
        if (LOWORD(b) != HTCLIENT) break;
        POINT point{}; GetCursorPos(&point); ScreenToClient(window, &point);
        if (hit(window, point).part == None) break;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW)); result = TRUE; return true;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        const auto target = hit(window, point);
        if (target.part == None) break;
        if (target.part == Corner) return true;
        const auto g = geometry(window, target.bar);
        if (g.maximum <= g.info.nMin) return true;
        SetFocus(window); pressed_ = hot_ = target; SetCapture(window);
        if (target.part == Thumb) {
            dragOffset_ = (target.bar == SB_VERT ? point.y : point.x) - g.thumbStart;
            trackPosition_ = g.info.nPos;
        } else {
            notify(window, target.part == Back ? SB_LINEUP : target.part == Forward ? SB_LINEDOWN : target.part == PageBack ? SB_PAGEUP : SB_PAGEDOWN);
            SetTimer(window, repeatTimer, 400, nullptr);
        }
        InvalidateRect(window, nullptr, FALSE); return true;
    }
    case WM_MOUSEMOVE: {
        if (GetCapture() == window && pressed_.part == None) break;
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        const auto target = hit(window, point);
        if (target.bar != hot_.bar || target.part != hot_.part) { hot_ = target; InvalidateRect(window, nullptr, FALSE); }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        if (pressed_.part == Thumb) {
            const auto g = geometry(window, pressed_.bar);
            const int travel = g.end - g.start - (g.thumbEnd - g.thumbStart);
            const int offset = std::clamp(static_cast<int>(pressed_.bar == SB_VERT ? point.y : point.x) - dragOffset_ - g.start, 0, std::max(0, travel));
            const auto span = static_cast<int64_t>(g.maximum) - g.info.nMin;
            trackPosition_ = g.info.nMin + (travel > 0 ? static_cast<int>((offset * span + travel / 2) / travel) : 0);
            notify(window, SB_THUMBTRACK);
        }
        return target.part != None || pressed_.part != None;
    }
    case WM_MOUSELEAVE:
        hot_ = {}; InvalidateRect(window, nullptr, FALSE); break;
    case WM_TIMER:
        if (a != repeatTimer || pressed_.part == None) break;
        SetTimer(window, repeatTimer, 50, nullptr);
        if (hot_.bar == pressed_.bar && hot_.part == pressed_.part) {
            POINT point{}; GetCursorPos(&point); ScreenToClient(window, &point);
            const auto target = hit(window, point);
            if (target.bar == pressed_.bar && target.part == pressed_.part)
                notify(window, pressed_.part == Back ? SB_LINEUP : pressed_.part == Forward ? SB_LINEDOWN : pressed_.part == PageBack ? SB_PAGEUP : SB_PAGEDOWN);
        }
        return true;
    case WM_LBUTTONUP:
        if (pressed_.part == None) break;
        if (pressed_.part == Thumb) notify(window, SB_THUMBPOSITION);
        notify(window, SB_ENDSCROLL); cancel(window); return true;
    case WM_CANCELMODE: case WM_CAPTURECHANGED: case WM_DESTROY:
        if (pressed_.part != None) cancel(window);
        break;
    case WM_SIZE: case WM_STYLECHANGED: {
        const auto g = geometry(window, pressed_.bar);
        if (pressed_.part != None && IsRectEmpty(&g.bounds)) cancel(window);
        break;
    }
    }
    return false;
}
}
