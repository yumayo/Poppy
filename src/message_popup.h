#pragma once
#include "viewer.h"

namespace poppy {
// Selectable, scrollable commit text. Opening on hover never takes focus.
class MessagePopup {
public:
    void show(HWND owner, const std::wstring& message, const RECT& anchor, UINT dpi);
    void hide(bool restoreFocus = true);
    void destroy();
    bool visible() const { return window_ && IsWindowVisible(window_); }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK bodyProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void tick();
    RECT copyBounds() const;
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    bool contains(HWND window) const { return window && (window == window_ || IsChild(window_, window)); }
    HWND window_ = nullptr, owner_ = nullptr, previousFocus_ = nullptr;
    Viewer body_;
    Renderer renderer_;
    UINT dpi_ = 96;
    RECT anchor_{};
    std::wstring message_;
    ULONGLONG outsideSince_ = 0;
    bool menuOpen_ = false, copyHot_ = false, copyPressed_ = false;
};
}
