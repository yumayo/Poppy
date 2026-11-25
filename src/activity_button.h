#pragma once
#include "render.h"

namespace poppy {
inline constexpr wchar_t activityButtonClassName[] = L"PoppyActivityButton";
class ActivityButton {
public:
    void create(HWND parent, HINSTANCE instance, const wchar_t* title, int id, Renderer::Icon icon);
    HWND window() const { return window_; }
    void setDpi(UINT dpi) { dpi_ = dpi; renderer_.setDpi(dpi); InvalidateRect(window_, nullptr, FALSE); }
    void select(bool value) { selected_ = value; InvalidateRect(window_, nullptr, FALSE); }
    void setBadgeCount(size_t count);
    void setTitle(std::wstring title);
    std::function<void()> clicked;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void activate();
    void cancel();
    bool contains(LPARAM point) const;
    HWND window_ = nullptr;
    Renderer renderer_;
    std::wstring title_;
    size_t badgeCount_ = 0;
    UINT dpi_ = 96;
    Renderer::Icon icon_ = Renderer::Icon::Explorer;
    bool selected_ = false, hover_ = false, mousePressed_ = false, keyPressed_ = false;
};
}
