#include "scrollbars.h"
#include "quick_open.h"
#include "viewer.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// WIC provides inspectable Direct2D pixels without a DC or GDI bitmap.
struct Bitmap {
    static constexpr int width = 512, height = 128;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    std::vector<BYTE> pixels = std::vector<BYTE>(width * height * 4);
    Bitmap() {
        check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory), reinterpret_cast<void**>(factory.put()))), "Could not create the WIC factory");
        check(SUCCEEDED(factory->CreateBitmap(width, height, GUID_WICPixelFormat32bppBGR, WICBitmapCacheOnLoad, bitmap.put())), "Could not create the WIC bitmap");
    }
    void read() { check(SUCCEEDED(bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())), "Could not read WIC pixels"); }
    RECT bounds() const { return {0, 0, width, height}; }
    COLORREF pixel(int x, int y) const {
        const auto offset = static_cast<size_t>((y * width + x) * 4);
        return RGB(pixels[offset + 2], pixels[offset + 1], pixels[offset]);
    }
};
void finish(Renderer& renderer) { check(SUCCEEDED(renderer.end()), "Direct2D drawing failed"); }
void drawCells(Renderer& renderer, Bitmap& bitmap, const std::vector<Cell>& cells, int cw, int ch) {
    check(renderer.begin(bitmap.bitmap.get()), "Could not bind Direct2D to the WIC bitmap");
    renderer.fill(bitmap.bounds(), background);
    renderer.cells(8, 8, cw, ch, cells, static_cast<int>(cells.size()), bitmap.bounds());
    finish(renderer); bitmap.read();
}
void cellPositions() {
    Renderer renderer, reference;
    Bitmap actual, expected;
    for (const UINT dpi : {96u, 144u, 192u}) {
        renderer.setDpi(dpi); reference.setDpi(dpi);
        const int cw = 24, ch = renderer.textHeight() + 4;
        std::vector<Cell> mixed(12), ascii(12);
        for (const auto index : {0, 3, 6, 9, 11}) mixed[index].ch = ascii[index].ch = U'A' + index;
        mixed[1].ch = U'日'; mixed[2].continuation = true;
        mixed[4].ch = U'\U0001f642'; mixed[5].continuation = true;
        mixed[7].ch = ascii[7].ch = U'f'; mixed[8].ch = ascii[8].ch = U'i';
        mixed[10].ch = U'\u05d0';
        drawCells(renderer, actual, mixed, cw, ch);
        drawCells(reference, expected, ascii, cw, ch);
        // Following glyphs must match an ASCII-only reference, even after a
        // fallback font, surrogate pair, RTL character, or potential ligature.
        for (const auto col : {0, 3, 6, 7, 8, 9, 11}) {
            bool ink = false;
            for (int y = 8; y < 8 + ch; ++y) for (int x = 8 + col * cw; x < 8 + (col + 1) * cw; ++x) {
                const auto pixel = expected.pixel(x, y);
                ink = ink || pixel != background;
                check(actual.pixel(x, y) == pixel, "Fallback text shifted a fixed-cell glyph or baseline");
            }
            check(ink, "DirectWrite did not draw the reference glyph");
        }
        // Brushes/targets are device resources; font metrics survive recreation.
        renderer.discard();
        drawCells(renderer, actual, ascii, cw, ch);
        for (int y = 0; y < Bitmap::height; ++y) for (int x = 0; x < Bitmap::width; ++x)
            check(actual.pixel(x, y) == expected.pixel(x, y), "Recreating the render target changed the drawing");
    }
}
void colorsAndClipping() {
    Renderer renderer; renderer.setDpi(96);
    Bitmap bitmap;
    check(renderer.begin(bitmap.bitmap.get()), "Could not create the color test target");
    const COLORREF untouched = RGB(201, 12, 193);
    renderer.fill(bitmap.bounds(), untouched);
    std::vector<Cell> cells(4);
    for (int i = 0; i < 4; ++i) cells[i].bg = RGB(20 + i * 40, 30, 80);
    cells[0].ch = U'日'; cells[1].continuation = true;
    const RECT clip{12, 12, 62, 38};
    renderer.cells(4, 4, 20, 40, cells, 4, clip);
    finish(renderer); bitmap.read();
    for (int y = 0; y < Bitmap::height; ++y) for (int x = 0; x < Bitmap::width; ++x) {
        if (x < clip.left || x >= clip.right || y < clip.top || y >= clip.bottom)
            check(bitmap.pixel(x, y) == untouched, "Cell drawing escaped its clip");
    }
    // Inspect below the glyphs, including a wide-character continuation whose
    // background differs from the leading cell (partial terminal selection).
    for (int x = clip.left; x < clip.right; ++x)
        check(bitmap.pixel(x, 36) == cells[(x - 4) / 20].bg, "A cell background or continuation color was lost");
    check(!renderer.begin(static_cast<IWICBitmap*>(nullptr)), "A null bitmap should not begin drawing");
    Bitmap other;
    check(renderer.begin(other.bitmap.get()), "Could not switch to another WIC bitmap");
    renderer.fill(other.bounds(), accent);
    finish(renderer); other.read(); bitmap.read();
    check(other.pixel(0, 0) == accent && other.pixel(511, 127) == accent, "A replacement WIC bitmap was not rendered");
    check(bitmap.pixel(0, 0) == untouched, "Switching WIC targets changed the old bitmap");
}
void smoothIcons() {
    Renderer renderer; Bitmap bitmap;
    for (const UINT dpi : {96u, 120u, 144u, 192u}) {
        renderer.setDpi(dpi);
        check(renderer.begin(bitmap.bitmap.get()), "Could not create the icon test target");
        renderer.fill(bitmap.bounds(), RGB(0, 0, 0));
        const int size = MulDiv(24, dpi, 96);
        const std::vector<Renderer::Icon> icons{Renderer::Icon::Explorer, Renderer::Icon::SourceControl, Renderer::Icon::Branch, Renderer::Icon::OpenAllChanges};
        for (size_t i = 0; i < icons.size(); ++i) {
            const int x = 8 + static_cast<int>(i) * 100;
            renderer.icon(icons[i], {x, 8, x + size, 8 + size}, RGB(255, 255, 255));
        }
        // Icon transforms must not leak into subsequent panel/cell fills.
        renderer.fill({440, 8, 460, 28}, accent);
        finish(renderer); bitmap.read();
        for (size_t i = 0; i < icons.size(); ++i) {
            const int first = 8 + static_cast<int>(i) * 100;
            bool ink = false, blendedEdge = false;
            for (int y = 7; y <= 8 + size; ++y) for (int x = first - 1; x <= first + size; ++x) {
                const auto pixel = bitmap.pixel(x, y);
                ink = ink || pixel != RGB(0, 0, 0);
                blendedEdge = blendedEdge || (pixel != RGB(0, 0, 0) && pixel != RGB(255, 255, 255));
            }
            check(ink && blendedEdge, "A vector icon is missing or has jagged, unblended edges");
        }
        for (int y = 7; y < 29; ++y) for (int x = 439; x < 461; ++x)
            check(bitmap.pixel(x, y) == (x >= 440 && x < 460 && y >= 8 && y < 28 ? accent : RGB(0, 0, 0)),
                "Icon scaling changed a subsequent fill or blurred an integer-aligned panel edge");
    }
}
void windowTargets() {
    HWND window = CreateWindowExW(0, L"STATIC", L"Poppy render test", WS_POPUP, 0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "Could not create a hidden render-test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{window};
    Renderer renderer; renderer.setDpi(96);
    const int width = renderer.cellWidth(), height = renderer.textHeight();
    renderer.setDpi(192);
    check(renderer.cellWidth() > width && renderer.textHeight() > height, "DPI changes did not update DirectWrite metrics");
    for (const auto size : {SIZE{320, 200}, SIZE{640, 360}, SIZE{0, 0}, SIZE{320, 200}}) {
        check(SetWindowPos(window, nullptr, 0, 0, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not resize the render-test window");
        const bool drawing = renderer.begin(window);
        check(drawing == (size.cx > 0 && size.cy > 0), "Window resize or zero-size handling failed");
        if (drawing) {
            renderer.fill({0, 0, size.cx, size.cy}, panel);
            renderer.label({8, 8, size.cx - 8, 80}, L"DirectWrite 日本語", foreground);
            finish(renderer);
        }
    }
    Bitmap bitmap;
    drawCells(renderer, bitmap, {Cell{U'M', foreground, background, false}}, 24, renderer.textHeight() + 4);
    check(renderer.begin(window), "Could not switch from a WIC target back to an HWND target");
    renderer.fill({0, 0, 320, 200}, accent);
    finish(renderer);
    renderer.discard();
    check(renderer.begin(window), "Could not recreate the HWND render target");
    finish(renderer);
}
void editorCaret() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Editor caret test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the editor test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr)); viewer.setDpi(96);
    const HWND window = viewer.window();
    check(MoveWindow(window, 0, 0, 640, 360, FALSE) != FALSE, "Could not size the editor");
    auto buffer = std::make_shared<EditBuffer>();
    indexDocument(buffer->document); buffer->initialize("");
    viewer.edit(buffer, L"caret.txt"); SetFocus(window);
    check(GetFocus() == window, "Could not focus the editor");
    const auto caret = [&] {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        check(GetGUIThreadInfo(GetCurrentThreadId(), &info) != FALSE, "Could not inspect the editor caret");
        check(info.hwndCaret == window, "The editor lost its native caret for IME/accessibility");
        check(!(info.flags & GUI_CARETBLINKING), "The native XOR caret is still drawing over Direct2D");
        return info.rcCaret;
    };
    const auto initial = caret();
    const LONG height = initial.bottom - initial.top;
    for (int row = 1; row <= 4; ++row) {
        SendMessageW(window, WM_CHAR, '\r', 0);
        SendMessageW(window, WM_PAINT, 0, 0);
        const auto current = caret();
        check(current.left == initial.left && current.top == initial.top + row * height,
            "A newline left the caret on the previous line");
    }
    for (int row = 0; row < 40; ++row) {
        SendMessageW(window, WM_CHAR, '\r', 0);
        SendMessageW(window, WM_PAINT, 0, 0);
        const auto current = caret();
        check(current.top >= initial.top && current.bottom <= Scrollbars::contentRect(window).bottom,
            "Newlines scrolled the caret outside the editor body");
    }
    check(viewer.state().caret == buffer->document.text.size(), "Repeated newlines lost the insertion point");
    SendMessageW(window, WM_KEYDOWN, VK_BACK, 0); caret();
    SendMessageW(window, WM_KEYDOWN, VK_UP, 0); caret();
    viewer.setDpi(144);
    check(caret().bottom - caret().top > height, "DPI changes did not resize the editor caret");

    // Use a real timer message so this also verifies that blinking was started.
    const UINT blink = GetCaretBlinkTime();
    MSG timer{};
    if (blink != INFINITE && blink <= 5000) {
        const auto start = GetTickCount64();
        while (!PeekMessageW(&timer, window, WM_TIMER, WM_TIMER, PM_REMOVE)) {
            check(GetTickCount64() - start < (blink ? blink : 500) + 1000, "The editor did not start its caret blink timer");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_TIMER);
        }
        for (int phase = 0; phase < 2; ++phase) {
            ValidateRect(window, nullptr); DispatchMessageW(&timer);
            check(GetUpdateRect(window, nullptr, FALSE) != FALSE, "Caret blinking did not request a Direct2D repaint");
            SendMessageW(window, WM_PAINT, 0, 0); caret();
        }
    }
    SetFocus(parent);
    check(GetUpdateRect(window, nullptr, FALSE) != FALSE, "Leaving the editor did not repaint its visible caret");
    ValidateRect(window, nullptr);
    if (timer.hwnd) DispatchMessageW(&timer);
    check(!GetUpdateRect(window, nullptr, FALSE), "A stale blink timer repainted an unfocused editor");
    SetFocus(window); caret();
    viewer.show(std::make_shared<Document>(buffer->document), L"read-only.txt");
    ValidateRect(window, nullptr);
    if (timer.hwnd) DispatchMessageW(&timer);
    check(!GetUpdateRect(window, nullptr, FALSE), "A stale blink timer repainted a read-only viewer");
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret != window,
        "A read-only viewer retained the editor caret");
}
struct ScrollbarWindow {
    Scrollbars bars;
    HWND window = nullptr;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
        auto* self = reinterpret_cast<ScrollbarWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<ScrollbarWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        LRESULT result = 0;
        if (self && self->bars.handle(window, message, a, b, result)) return result;
        if (self && (message == WM_VSCROLL || message == WM_HSCROLL)) {
            const int bar = message == WM_VSCROLL ? SB_VERT : SB_HORZ;
            auto info = self->info(window, bar);
            switch (LOWORD(a)) {
            case SB_THUMBTRACK: case SB_THUMBPOSITION: info.nPos = self->bars.trackPosition(bar, info.nTrackPos); break;
            case SB_LINEUP: --info.nPos; break; case SB_LINEDOWN: ++info.nPos; break;
            case SB_PAGEUP: info.nPos -= static_cast<int>(info.nPage); break;
            case SB_PAGEDOWN: info.nPos += static_cast<int>(info.nPage); break;
            }
            info.fMask = SIF_POS; SetScrollInfo(window, bar, &info, FALSE); return 0;
        }
        return DefWindowProcW(window, message, a, b);
    }
    static SCROLLINFO info(HWND window, int bar) {
        SCROLLINFO value{sizeof(value), SIF_ALL, 0, 0, 0, 0, 0};
        check(GetScrollInfo(window, bar, &value) != FALSE, "The themed scrollbar lost its Win32 scroll information");
        return value;
    }
    ScrollbarWindow() {
        WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"PoppyScrollbarTest";
        RegisterClassW(&cls);
        window = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP | WS_VSCROLL | WS_HSCROLL,
            0, 0, 320, 128, nullptr, nullptr, cls.hInstance, this);
        check(window != nullptr, "Could not create the scrollbar test window");
        for (const int bar : {SB_VERT, SB_HORZ}) {
            SCROLLINFO value{sizeof(value), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 999999, 100, 0, 0};
            SetScrollInfo(window, bar, &value, FALSE);
        }
    }
    ~ScrollbarWindow() { DestroyWindow(window); }
};
struct ViewerPaintWatch {
    HWND window;
    WNDPROC original;
    size_t paints = 0;
    POINT painted{};
    explicit ViewerPaintWatch(HWND value) : window(value) {
        check(SetPropW(window, L"PoppyViewerPaintWatch", this) != FALSE, "Could not observe viewer paints");
        original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure)));
        check(original != nullptr, "Could not subclass the viewer");
    }
    ~ViewerPaintWatch() {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        RemovePropW(window, L"PoppyViewerPaintWatch");
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
        auto& self = *static_cast<ViewerPaintWatch*>(GetPropW(window, L"PoppyViewerPaintWatch"));
        const auto result = CallWindowProcW(self.original, window, message, a, b);
        if (message == WM_PAINT) {
            ++self.paints;
            self.painted = {ScrollbarWindow::info(window, SB_HORZ).nPos, ScrollbarWindow::info(window, SB_VERT).nPos};
        }
        return result;
    }
};
void continuousViewerScrolling() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Continuous scrolling test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the scrolling test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr)); viewer.setDpi(96);
    const HWND window = viewer.window();
    check(MoveWindow(window, 0, 0, 640, 360, FALSE) != FALSE, "Could not size the scrolling test viewer");
    auto buffer = std::make_shared<EditBuffer>();
    const auto line = std::string(maxLineColumns, 'x') + '\n';
    for (int row = 0; row < 256; ++row) buffer->document.text += line;
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"long-lines.html");
    ShowWindow(parent, SW_SHOWNOACTIVATE); SetFocus(window);
    ViewerPaintWatch observed(window);
    const auto checkPaint = [&](size_t before) {
        check(observed.paints > before, "Continuous input deferred the viewer paint until scrolling stopped");
        check(observed.painted.x == viewer.state().left && observed.painted.y == viewer.state().top,
            "The viewer painted an outdated scroll position");
    };
    for (const int bar : {SB_HORZ, SB_VERT}) {
        viewer.restore({}); UpdateWindow(window);
        RECT client{}; GetClientRect(window, &client);
        const auto content = Scrollbars::contentRect(window);
        const int cross = bar == SB_VERT ? (content.right + client.right) / 2 : (content.bottom + client.bottom) / 2;
        const int arrow = bar == SB_VERT ? client.right - content.right : client.bottom - content.bottom;
        const auto point = [&](int along) { return bar == SB_VERT ? MAKELPARAM(cross, along) : MAKELPARAM(along, cross); };
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(arrow + 1));
        check(GetCapture() == window, "The viewer did not capture its scrollbar thumb");
        const int length = bar == SB_VERT ? content.bottom : content.right;
        // Keep input queued, with jumps large enough to replace the visible
        // text. Do not dispatch a queued WM_PAINT between mouse moves.
        for (int step = 1; step <= 10; ++step)
            check(PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, point(arrow + 1 + length * step / 10)) != FALSE,
                "Could not queue a continuous thumb drag");
        MSG message{}; size_t moved = 0;
        while (PeekMessageW(&message, window, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) {
            const int before = ScrollbarWindow::info(window, bar).nPos;
            const auto paints = observed.paints; DispatchMessageW(&message);
            if (ScrollbarWindow::info(window, bar).nPos != before) { checkPaint(paints); ++moved; }
        }
        check(moved >= 6, "The drag did not exercise enough distinct large scroll positions");
        SendMessageW(window, WM_LBUTTONUP, 0, point(length));
        check(GetCapture() != window, "Continuous scrolling retained mouse capture");
    }
    viewer.restore({}); UpdateWindow(window);
    for (int step = 0; step < 8; ++step) {
        const auto paints = observed.paints;
        SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        checkPaint(paints);
    }
}
void themedScrollbars() {
    ScrollbarWindow pane;
    Renderer renderer; renderer.setDpi(96);
    Bitmap bitmap;
    const auto draw = [&] {
        check(renderer.begin(bitmap.bitmap.get()), "Could not render the themed scrollbars");
        renderer.fill(bitmap.bounds(), accent);
        pane.bars.paint(pane.window, renderer, panel);
        finish(renderer); bitmap.read();
    };
    RECT client{}; GetClientRect(pane.window, &client);
    check(client.right == 320 && client.bottom == 128, "Native nonclient scrollbars still consume the custom bars' client area");
    const auto content = Scrollbars::contentRect(pane.window);
    check(content.right < client.right && content.bottom < client.bottom, "The bars overlap the content area");
    draw();
    check(bitmap.pixel(0, 0) == accent, "Scrollbar painting changed the document area");
    check(bitmap.pixel(client.right - 1, client.bottom - 1) == panel, "The scrollbar corner does not match its pane");
    check(bitmap.pixel(content.right, content.bottom / 2) == panel && bitmap.pixel(content.right / 2, content.bottom) == panel,
        "A scrollbar track does not match its pane");
    const auto checkMouseTarget = [&](POINT point) {
        ClientToScreen(pane.window, &point);
        check(SendMessageW(pane.window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) == HTCLIENT,
            "A themed scrollbar routes real mouse input to native nonclient tracking");
    };
    checkMouseTarget({client.right - 1, client.bottom - 1});
    for (const int bar : {SB_VERT, SB_HORZ}) {
        draw();
        const int cross = bar == SB_VERT ? (content.right + client.right) / 2 : (content.bottom + client.bottom) / 2;
        const int length = bar == SB_VERT ? content.bottom : content.right;
        int first = -1, last = -1;
        for (int along = 0; along < length; ++along) {
            if ((bar == SB_VERT ? bitmap.pixel(cross, along) : bitmap.pixel(along, cross)) == scrollbarThumb) {
                if (first < 0) first = along;
                last = along;
            }
        }
        check(first >= 0 && last > first, "A scrollbar thumb is missing its theme color");
        const int along = (first + last) / 2;
        const POINT point = bar == SB_VERT ? POINT{cross, along} : POINT{along, cross};
        const LPARAM location = MAKELPARAM(point.x, point.y);
        checkMouseTarget(point);
        SendMessageW(pane.window, WM_MOUSEMOVE, 0, location); draw();
        check(bitmap.pixel(point.x, point.y) == scrollbarHover, "Hovering a thumb did not update its theme color");
        SendMessageW(pane.window, WM_LBUTTONDOWN, MK_LBUTTON, location); draw();
        check(GetCapture() == pane.window && bitmap.pixel(point.x, point.y) == muted, "Dragging a thumb did not capture the pointer or update its color");
        const POINT end = bar == SB_VERT ? POINT{cross, client.bottom + 100} : POINT{client.right + 100, cross};
        SendMessageW(pane.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(end.x, end.y));
        check(ScrollbarWindow::info(pane.window, bar).nPos == 999900, "Dragging truncated a scroll position above 65535 or did not reach the end");
        SendMessageW(pane.window, WM_LBUTTONUP, 0, MAKELPARAM(end.x, end.y));
        check(GetCapture() != pane.window, "Releasing the thumb left the pointer captured");
        checkMouseTarget(bar == SB_VERT ? POINT{cross, 2} : POINT{2, cross});
        SendMessageW(pane.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(bar == SB_VERT ? cross : 2, bar == SB_VERT ? 2 : cross));
        check(ScrollbarWindow::info(pane.window, bar).nPos == 999899, "The scrollbar arrow did not scroll one line");
        SendMessageW(pane.window, WM_CANCELMODE, 0, 0);
        check(GetCapture() != pane.window, "Cancelling a scrollbar press left the pointer captured");
        SendMessageW(pane.window, WM_LBUTTONDOWN, MK_LBUTTON, location);
        check(ScrollbarWindow::info(pane.window, bar).nPos == 999799, "Clicking the scrollbar track did not scroll one page");
        SendMessageW(pane.window, WM_LBUTTONUP, 0, location);
    }
    for (const int bar : {SB_VERT, SB_HORZ}) {
        SCROLLINFO value{sizeof(value), SIF_RANGE | SIF_PAGE, 0, 0, 1, 0, 0};
        SetScrollInfo(pane.window, bar, &value, FALSE);
    }
    const auto empty = Scrollbars::contentRect(pane.window);
    check(empty.right == client.right && empty.bottom == client.bottom, "Hidden bars did not return their space to the content");
}
// Observe every intermediate window change on the UI thread, so a shrink and
// expand within a few milliseconds cannot escape the regression check.
struct QuickOpenWatch {
    HWND window;
    WNDPROC original;
    RECT bounds{};
    int geometryChanges = 0, unexpectedHides = 0, completions = 0;
    bool allowClose = false;
    explicit QuickOpenWatch(HWND value) : window(value) {
        GetWindowRect(window, &bounds);
        MapWindowPoints(nullptr, GetParent(window), reinterpret_cast<POINT*>(&bounds), 2);
        check(SetPropW(window, L"PoppyQuickOpenWatch", this) != FALSE, "Could not observe quick open");
        original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure)));
        check(original != nullptr, "Could not subclass quick open");
    }
    ~QuickOpenWatch() {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        RemovePropW(window, L"PoppyQuickOpenWatch");
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
        auto& self = *static_cast<QuickOpenWatch*>(GetPropW(window, L"PoppyQuickOpenWatch"));
        if (message == WM_WINDOWPOSCHANGING) {
            const auto position = reinterpret_cast<WINDOWPOS*>(b);
            if ((!(position->flags & SWP_NOSIZE) && (position->cx != self.bounds.right - self.bounds.left || position->cy != self.bounds.bottom - self.bounds.top)) ||
                (!(position->flags & SWP_NOMOVE) && (position->x != self.bounds.left || position->y != self.bounds.top))) ++self.geometryChanges;
        }
        if (message == WM_SHOWWINDOW && !a && !self.allowClose) ++self.unexpectedHides;
        const auto result = CallWindowProcW(self.original, window, message, a, b);
        if (message == wmWork) ++self.completions;
        return result;
    }
};
void quickOpenStability() {
    struct Files {
        fs::path root = fs::temp_directory_path() / (L"Poppy popup test " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
        Files() {
            check(fs::create_directory(root), "Could not create popup test files");
            for (const auto name : {L"alpha.txt", L"beta.txt", L"gamma.txt"}) {
                std::ofstream file(root / name); file << "test\n";
                check(file.good(), "Could not write a popup test file");
            }
        }
        ~Files() { std::error_code error; fs::remove_all(root, error); }
    } files;
    QuickOpen palette;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Quick open test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the popup test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    palette.create(parent, GetModuleHandleW(nullptr)); palette.setDpi(96);
    fs::path opened;
    palette.opened = [&](const ResourceId& path) { opened = path.local(); };
    const auto wait = [](const auto& done) {
        const auto start = GetTickCount64();
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (done()) return;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        } while (GetTickCount64() - start < 5000);
        throw std::runtime_error("Quick open did not finish updating");
    };
    const auto accept = [&] { SendMessageW(palette.window(), WM_KEYDOWN, VK_RETURN, 0); return !palette.visible(); };
    palette.setRoot(files.root); palette.show(); wait(accept); // Warm the startup list.
    palette.show();
    QuickOpenWatch observed(palette.window());
    for (const auto ch : std::wstring(L"alpha")) SendMessageW(palette.window(), WM_CHAR, ch, 0);
    observed.allowClose = true; wait(accept); observed.allowClose = false;
    check(opened == files.root / L"alpha.txt", "Quick open opened an outdated match");
    palette.show();
    const auto completed = observed.completions;
    SendMessageW(palette.window(), WM_CHAR, L'z', 0);
    wait([&] { return observed.completions > completed; });
    SendMessageW(palette.window(), WM_KEYDOWN, VK_RETURN, 0);
    check(palette.visible(), "An empty result closed quick open");
    SendMessageW(palette.window(), WM_KEYDOWN, VK_BACK, 0);
    SendMessageW(palette.window(), WM_KEYDOWN, VK_DOWN, 0);
    observed.allowClose = true;
    check(accept() && opened == files.root / L"beta.txt", "Quick open lost keyboard selection after clearing the query");
    check(!observed.geometryChanges && !observed.unexpectedHides, "Typing or completing a search resized or hid quick open");
}
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    try {
        initializeRendering();
        cellPositions(); colorsAndClipping(); smoothIcons(); windowTargets(); editorCaret(); continuousViewerScrolling(); themedScrollbars(); quickOpenStability();
        std::cout << "Direct2D rendering tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
