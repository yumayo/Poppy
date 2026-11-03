#include "error_popup.h"
#include <algorithm>
#include <dwmapi.h>

namespace poppy {
void ErrorPopup::show(HWND owner, const std::string& key, const std::wstring& title, const std::wstring& message, UINT dpi) {
    if (std::any_of(entries_.begin(), entries_.end(), [&](const Entry& entry) { return entry.key == key; })) return;
    if (!window_) {
        owner_ = GetAncestor(owner, GA_ROOT);
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner_, GWLP_HINSTANCE));
        WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance;
        cls.lpszClassName = L"PoppyErrorPopup"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Poppy — エラー通知",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, 0, 0, owner_, nullptr, instance, this);
        if (!window_) return;
        const BOOL dark = TRUE; DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
        body_.create(window_, instance, true);
    }
    // Keep existing notices; repeated failures cannot grow the document forever.
    if (entries_.size() < 64) entries_.push_back({key, title.substr(0, 256), message.substr(0, 8192)});
    else ++overflow_;
    setDpi(dpi);
    updateText();
    layout();
    ShowWindow(window_, SW_SHOWNOACTIVATE);
}
void ErrorPopup::setDpi(UINT dpi) {
    dpi_ = dpi;
    if (window_) { body_.setDpi(dpi); layout(); }
}
void ErrorPopup::updateText() {
    auto document = std::make_shared<Document>();
    std::wstring text;
    for (const auto& entry : entries_) {
        if (!text.empty()) text += L"\n\n────────────────\n\n";
        text += entry.title + L"\n\n" + entry.message;
    }
    if (overflow_) text += L"\n\nほか " + std::to_wstring(overflow_) + L" 件のエラーが発生しました。";
    text += L"\n\nこの通知は右上の ✕ で閉じるまで残ります。";
    document->text = utf8(text); indexDocument(*document);
    body_.show(std::move(document), {}, false, false);
    SetWindowTextW(window_, (L"Poppy — エラー通知 (" + std::to_wstring(entries_.size() + overflow_) + L")").c_str());
}
void ErrorPopup::layout() {
    if (!window_ || !IsWindow(owner_) || IsIconic(owner_)) return;
    RECT client{}; GetClientRect(owner_, &client);
    POINT anchor{client.right, client.bottom}; ClientToScreen(owner_, &anchor);
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
    GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto& work = monitor.rcWork;
    const int width = std::min(px(440), static_cast<int>(work.right - work.left));
    const int height = std::min(px(260), static_cast<int>(work.bottom - work.top));
    const int x = std::clamp(static_cast<int>(anchor.x) - width - px(16), static_cast<int>(work.left), static_cast<int>(work.right) - width);
    const int y = std::clamp(static_cast<int>(anchor.y) - height - px(16), static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    SetWindowPos(window_, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
}
void ErrorPopup::destroy() {
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    entries_.clear(); overflow_ = 0;
}
LRESULT CALLBACK ErrorPopup::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<ErrorPopup*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ErrorPopup*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, a, b);
    const auto result = self->handle(message, a, b);
    if (message == WM_NCDESTROY) { SetWindowLongPtrW(window, GWLP_USERDATA, 0); self->window_ = nullptr; }
    return result;
}
LRESULT ErrorPopup::handle(UINT message, WPARAM a, LPARAM b) {
    switch (message) {
    case WM_CLOSE:
        ShowWindow(window_, SW_HIDE); entries_.clear(); overflow_ = 0; body_.message(L""); return 0;
    case WM_SIZE: {
        RECT client{}; GetClientRect(window_, &client);
        if (body_.window()) MoveWindow(body_.window(), 0, 0, client.right, client.bottom, TRUE);
        return 0;
    }
    case WM_DPICHANGED: setDpi(HIWORD(a)); return 0;
    case WM_ERASEBKGND: return 1;
    default: return DefWindowProcW(window_, message, a, b);
    }
}
}
