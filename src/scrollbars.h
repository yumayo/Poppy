#pragma once
#include "render.h"

namespace poppy {
// Keep Win32 scroll ranges/positions, but draw and hit-test the bars in the
// client area so their colors do not depend on the Windows light/dark theme.
class Scrollbars {
public:
    static RECT contentRect(HWND window);
    void paint(HWND window, Renderer& renderer, COLORREF surface);
    bool handle(HWND window, UINT message, WPARAM a, LPARAM b, LRESULT& result);
    int trackPosition(int bar, int fallback) const;
private:
    enum Part { None, Back, PageBack, Thumb, PageForward, Forward, Corner };
    struct Hit { int bar = SB_VERT; Part part = None; };
    struct Geometry {
        RECT bounds{};
        int start = 0, end = 0, thumbStart = 0, thumbEnd = 0;
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL, 0, 0, 0, 0, 0};
        int maximum = 0;
    };
    static Geometry geometry(HWND window, int bar);
    static Hit hit(HWND window, POINT point);
    void notify(HWND window, UINT code);
    void cancel(HWND window);
    Hit hot_, pressed_;
    int dragOffset_ = 0, trackPosition_ = 0;
    static constexpr UINT_PTR repeatTimer = 0x5053;
};
}
