#include "scrollbars.h"
#include <algorithm>
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
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    try {
        initializeRendering();
        cellPositions(); colorsAndClipping(); windowTargets(); themedScrollbars();
        std::cout << "Direct2D rendering tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
