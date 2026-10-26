#pragma once
#include "render.h"
#include <optional>

namespace poppy {
class FindBar {
public:
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void layout(int top, int width, int height);
    void show(const std::optional<std::wstring>& seed = {}, bool focus = true);
    void projectSearch() { compact_ = true; }
    void close(bool restoreFocus = true);
    bool visible() const { return visible_; }
    HWND window() const { return window_; }
    const std::wstring& query() const { return query_; }
    bool matchCase() const { return matchCase_; }
    void status(std::wstring text);
    std::function<void()> changed, closed, focused;
    std::function<void()> accepted, escaped;
    std::function<void(bool)> navigate;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void replace(std::wstring text);
    void caretChanged();
    void imePosition();
    size_t previous(size_t position) const;
    size_t next(size_t position) const;
    RECT input() const;
    RECT button(int index) const;
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    HWND window_ = nullptr;
    Renderer renderer_;
    UINT dpi_ = 96;
    std::wstring query_, status_;
    size_t caret_ = 0, anchor_ = 0, inputFirst_ = 0;
    int caretX_ = 0;
    wchar_t highSurrogate_ = 0;
    bool visible_ = false, composing_ = false, matchCase_ = false, compact_ = false;
};
}
