#pragma once
#include "viewer.h"

namespace poppy {
// Nonmodal errors remain available until the user closes the small window.
class ErrorPopup {
public:
    ~ErrorPopup() { destroy(); }
    void show(HWND owner, const std::string& key, const std::wstring& title, const std::wstring& message, UINT dpi);
    void setDpi(UINT dpi);
    void layout();
    void destroy();
    HWND window() const { return window_; }
    bool visible() const { return window_ && IsWindowVisible(window_); }
    bool contains(HWND window) const { return window_ && (window == window_ || IsChild(window_, window)); }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void updateText();
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    struct Entry { std::string key; std::wstring title, message; };
    std::vector<Entry> entries_;
    size_t overflow_ = 0;
    HWND window_ = nullptr, owner_ = nullptr;
    Viewer body_;
    UINT dpi_ = 96;
};
}
