#include "status_bar.h"
#include <commctrl.h>
#include <algorithm>

namespace poppy {
void StatusBar::create(HWND parent, HINSTANCE instance) {
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance;
    cls.lpszClassName = L"PoppyStatusBar"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Project status", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr, instance, this);
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance, nullptr);
    SendMessageW(tooltips_, TTM_SETMAXTIPWIDTH, 0, px(560));
}
void StatusBar::setDpi(UINT dpi) { dpi_ = dpi; renderer_.setDpi(dpi); SendMessageW(tooltips_, TTM_SETMAXTIPWIDTH, 0, px(560)); layout(); }
void StatusBar::message(std::wstring text) {
    if (text == message_) return;
    message_ = std::move(text); if (window_) InvalidateRect(window_, nullptr, FALSE);
}
void StatusBar::update(std::wstring git, std::vector<LimitStatus> limits) {
    if (git_ == git && limits_ == limits) return;
    git_ = std::move(git); limits_ = std::move(limits); layout();
}
void StatusBar::layout() {
    if (!window_) return;
    RECT client{}; GetClientRect(window_, &client);
    const int padding = px(10), gap = px(12), overflowWidth = renderer_.textWidth(L"Limits") + 2 * padding;
    // Preserve room for Git and operation messages. Less important counters
    // move into a Limits tooltip when the window is narrow.
    const int budget = std::max(0L, client.right - px(320));
    int used = 0; size_t visible = 0;
    for (size_t i = 0; i < limits_.size(); ++i) {
        const int width = renderer_.textWidth(limits_[i].text) + 2 * padding;
        if (used + width + (i + 1 < limits_.size() ? overflowWidth : 0) > budget) break;
        used += width; ++visible;
    }
    for (size_t i = 0; i < tools_; ++i) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.hwnd = window_; tip.uId = i + 1;
        SendMessageW(tooltips_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    parts_.clear();
    int edge = std::max(0L, client.right - padding);
    if (visible < limits_.size()) {
        std::wstring tip; bool warning = false;
        for (size_t i = visible; i < limits_.size(); ++i) { if (!tip.empty()) tip += L"\n\n"; tip += limits_[i].text + L"\n" + limits_[i].tooltip; warning |= limits_[i].warning; }
        const int left = std::max(0, edge - overflowWidth);
        parts_.push_back({{left, 0, edge, client.bottom}, L"Limits", std::move(tip), warning}); edge = left;
    }
    for (size_t i = visible; i > 0; --i) {
        const auto& item = limits_[i - 1]; const int width = renderer_.textWidth(item.text) + 2 * padding;
        parts_.push_back({{edge - width, 0, edge, client.bottom}, item.text, item.tooltip, item.warning}); edge -= width;
    }
    const int gitWidth = std::min(px(300), std::max(px(100), (edge - 2 * padding) / 2));
    gitBounds_ = {std::min(edge, padding), 0, std::min<LONG>(edge, padding + gitWidth), client.bottom};
    const int messageRight = std::max(0, edge - gap);
    messageBounds_ = {std::min<LONG>(messageRight, gitBounds_.right + gap), 0, messageRight, client.bottom};
    // Separate tools also expose full messages when their labels are clipped.
    parts_.push_back({gitBounds_, {}, git_, false}); parts_.push_back({messageBounds_, {}, message_, false});
    tools_ = parts_.size();
    for (size_t i = 0; i < tools_; ++i) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.hwnd = window_; tip.uId = i + 1; tip.uFlags = TTF_SUBCLASS;
        tip.rect = parts_[i].bounds; tip.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void StatusBar::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    RECT client{}; GetClientRect(window_, &client); renderer_.fill(client, panel);
    renderer_.label(gitBounds_, git_, muted);
    if (messageBounds_.right > messageBounds_.left) renderer_.label(messageBounds_, message_, muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (const auto& part : parts_) if (!part.text.empty()) {
        const auto& r = part.bounds; renderer_.fill({r.left, px(5), r.left + 1, r.bottom - px(5)}, border);
        renderer_.label({r.left + px(10), r.top, r.right - px(10), r.bottom}, part.text, part.warning ? RGB(224, 192, 128) : muted);
    }
}
LRESULT CALLBACK StatusBar::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<StatusBar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<StatusBar*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(message, a, b) : DefWindowProcW(window, message, a, b);
}
LRESULT StatusBar::handle(UINT message, WPARAM a, LPARAM b) {
    switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: layout(); return 0;
    case WM_NOTIFY: {
        const auto info = reinterpret_cast<NMTTDISPINFOW*>(b);
        if (info->hdr.hwndFrom == tooltips_ && info->hdr.code == TTN_GETDISPINFOW && info->hdr.idFrom > 0 && info->hdr.idFrom <= parts_.size()) {
            const auto index = info->hdr.idFrom - 1;
            tipText_ = index + 1 == parts_.size() ? message_ : parts_[index].tooltip;
            info->lpszText = tipText_.data();
        }
        return 0;
    }
    default: return DefWindowProcW(window_, message, a, b);
    }
}
}
