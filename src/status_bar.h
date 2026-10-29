#pragma once
#include "render.h"
#include "limit_status.h"

namespace poppy {
class StatusBar {
public:
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void message(std::wstring text);
    void update(std::wstring git, std::vector<LimitStatus> limits);
    HWND window() const { return window_; }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void layout();
    void paint();
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    struct Part { RECT bounds; std::wstring text, tooltip; bool warning; };
    HWND window_ = nullptr, tooltips_ = nullptr;
    Renderer renderer_;
    UINT dpi_ = 96;
    std::wstring git_, message_, tipText_;
    std::vector<LimitStatus> limits_;
    std::vector<Part> parts_;
    RECT gitBounds_{}, messageBounds_{};
    size_t tools_ = 0;
};
}
