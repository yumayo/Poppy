#include "viewer.h"
#include <algorithm>
#include <array>

namespace poppy {
namespace {
constexpr std::array<Color, 6> styles{foreground, rgb(191,151,220), rgb(151,195,121), rgb(107,129,145), rgb(224,192,128), rgb(231,112,118)};
}
Viewer::~Viewer() { if (font_) DeleteObject(font_); if (uiFont_) DeleteObject(uiFont_); }
void Viewer::create(HWND parent, HINSTANCE instance) {
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyViewer"; cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Code viewer", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, instance, this);
    setDpi(GetDpiForWindow(parent));
}
void Viewer::setDpi(UINT dpi) {
    if (font_) DeleteObject(font_); if (uiFont_) DeleteObject(uiFont_);
    font_ = makeFont(MulDiv(15, dpi, 96)); uiFont_ = makeFont(MulDiv(13, dpi, 96), false);
    HDC dc = GetDC(window_); const auto old = SelectObject(dc, font_); TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics); SelectObject(dc, old); ReleaseDC(window_, dc);
    cellWidth_ = metrics.tmAveCharWidth; cellHeight_ = metrics.tmHeight + MulDiv(3, dpi, 96);
    header_ = MulDiv(34, dpi, 96); gutter_ = MulDiv(70, dpi, 96);
    scrollbars(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::show(std::shared_ptr<const Document> d, const fs::path& path, bool preserve) {
    document_ = std::move(d); path_ = path; message_.clear();
    if (!preserve) { top_ = left_ = 0; anchor_ = caret_ = 0; }
    else { anchor_ = std::min(anchor_, document_->text.size()); caret_ = std::min(caret_, document_->text.size()); }
    scrollbars(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::message(const std::wstring& text, const fs::path& path) {
    document_.reset(); path_ = path; message_ = text; top_ = left_ = 0; anchor_ = caret_ = 0;
    scrollbars(); InvalidateRect(window_, nullptr, FALSE);
}
int Viewer::visibleRows() const { RECT r{}; GetClientRect(window_, &r); return std::max(1, (static_cast<int>(r.bottom) - header_) / cellHeight_); }
void Viewer::scrollbars() {
    if (!window_) return;
    RECT r{}; GetClientRect(window_, &r);
    const int rows = document_ ? static_cast<int>(document_->lines.size()) : 1;
    const int cols = document_ ? static_cast<int>(document_->maxColumns) : 1;
    const int pageRows = visibleRows(), pageCols = std::max(1, (static_cast<int>(r.right) - gutter_) / cellWidth_);
    top_ = std::clamp(top_, 0, std::max(0, rows - pageRows)); left_ = std::clamp(left_, 0, std::max(0, cols - pageCols));
    SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0, rows - 1, static_cast<UINT>(pageRows), top_, 0}; SetScrollInfo(window_, SB_VERT, &vertical, TRUE);
    SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0, cols - 1, static_cast<UINT>(pageCols), left_, 0}; SetScrollInfo(window_, SB_HORZ, &horizontal, TRUE);
}
void Viewer::scroll(int vertical, int horizontal) { top_ += vertical; left_ += horizontal; scrollbars(); InvalidateRect(window_, nullptr, FALSE); }
size_t Viewer::position(int x, int y) const {
    if (!document_) return 0;
    const size_t row = std::min(document_->lines.size() - 1, static_cast<size_t>(std::max(0, (y - header_) / cellHeight_ + top_)));
    const int target = std::max(0, (x - gutter_ + cellWidth_ / 2) / cellWidth_ + left_);
    const auto line = document_->line(row); int col = 0; size_t p = 0;
    while (p < line.size() && col < target) {
        const size_t before = p; const char32_t c = decodeUtf8(line, p);
        const int width = c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c));
        if (col + width > target) { p = before; break; } col += width;
    }
    return document_->lines[row] + p;
}
void Viewer::copy() {
    if (!document_ || anchor_ == caret_) return;
    const size_t start = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
    copyText(window_, wide(std::string_view(document_->text).substr(start, end - start)));
}
void Viewer::paint() {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(window_, &ps); RECT r{}; GetClientRect(window_, &r);
    const auto oldFont = SelectObject(dc, uiFont_);
    RECT header{0, 0, r.right, header_}; fill(dc, header, panel);
    RECT title{14, 0, std::max<LONG>(14, r.right - 175), header_};
    label(dc, title, path_.empty() ? L"POPPY" : path_.filename().native(), foreground);
    RECT tag{std::max<LONG>(14, r.right - 175), 0, r.right - 12, header_};
    label(dc, tag, document_ ? wide(document_->encoding) + L"  ·  READ ONLY" : L"READ ONLY", muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    fill(dc, {0, header_ - 1, r.right, header_}, border);
    if (!document_) {
        fill(dc, {0, header_, r.right, r.bottom}, background);
        RECT text{32, header_ + 48, std::max<LONG>(32, r.right - 32), r.bottom};
        label(dc, text, message_, muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        SelectObject(dc, font_);
        const int columns = std::max(0, (static_cast<int>(r.right) - gutter_) / cellWidth_ + 1);
        const int rows = std::max(0, (static_cast<int>(r.bottom) - header_ + cellHeight_ - 1) / cellHeight_);
        const size_t selectionStart = std::min(anchor_, caret_), selectionEnd = std::max(anchor_, caret_);
        std::vector<Token> tokens; std::vector<Cell> cells(static_cast<size_t>(columns));
        for (int screenRow = 0; screenRow < rows; ++screenRow) {
            const int y = header_ + screenRow * cellHeight_; const size_t row = static_cast<size_t>(top_ + screenRow);
            fill(dc, {0, y, gutter_, y + cellHeight_}, background);
            fill(dc, {gutter_, y, r.right, y + cellHeight_}, background);
            if (row >= document_->lines.size()) continue;
            RECT number{0, y, gutter_ - 14, y + cellHeight_}; label(dc, number, std::to_wstring(row + 1), muted, DT_RIGHT | DT_SINGLELINE);
            auto line = document_->line(row); line = line.substr(0, std::min(line.size(), maxLineColumns * 4));
            LexState state = document_->states[row]; lexLine(line, document_->language, state, &tokens);
            std::fill(cells.begin(), cells.end(), Cell{});
            int col = 0; size_t token = 0;
            for (size_t p = 0; p < line.size() && col < left_ + columns && col < static_cast<int>(maxLineColumns);) {
                const size_t begin = p; char32_t c = decodeUtf8(line, p);
                const int width = c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c));
                while (token < tokens.size() && tokens[token].end <= begin) ++token;
                const auto style = token < tokens.size() && tokens[token].begin <= begin ? tokens[token].style : Style::Text;
                const size_t offset = document_->lines[row] + begin;
                const Color bg = offset >= selectionStart && offset < selectionEnd ? rgb(50, 70, 94) : background;
                if (c < 32) c = U' ';
                for (int i = 0; i < width; ++i) {
                    const int x = col + i - left_; if (x < 0 || x >= columns) continue;
                    const bool wideContinuation = cellWidth(c) == 2 && i == 1;
                    cells[x] = {i == 0 ? c : U' ', styles[static_cast<size_t>(style)], bg, wideContinuation};
                    if (i > 0 && col < left_) cells[x] = {U' ', foreground, bg, false};
                }
                col += width;
            }
            RECT clip{gutter_, header_, r.right, r.bottom}; paintCells(dc, gutter_, y, cellWidth_, cellHeight_, cells, columns, clip);
        }
    }
    SelectObject(dc, oldFont); EndPaint(window_, &ps);
}
LRESULT CALLBACK Viewer::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<Viewer*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT Viewer::handle(UINT m, WPARAM a, LPARAM b) {
    switch (m) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: scrollbars(); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN:
        SetFocus(window_); if (GET_Y_LPARAM(b) < header_) return 0;
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) anchor_ = position(GET_X_LPARAM(b), GET_Y_LPARAM(b));
        caret_ = position(GET_X_LPARAM(b), GET_Y_LPARAM(b)); selecting_ = true; SetCapture(window_); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE: if (selecting_) { caret_ = position(GET_X_LPARAM(b), GET_Y_LPARAM(b)); InvalidateRect(window_, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: if (selecting_) { selecting_ = false; ReleaseCapture(); } return 0;
    case WM_CAPTURECHANGED: selecting_ = false; return 0;
    case WM_MOUSEWHEEL: scroll(-GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * 3, 0); return 0;
    case WM_VSCROLL: case WM_HSCROLL: {
        const bool vertical = m == WM_VSCROLL; int& pos = vertical ? top_ : left_;
        SCROLLINFO info{sizeof(info), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window_, vertical ? SB_VERT : SB_HORZ, &info);
        switch (LOWORD(a)) {
        case SB_LINEUP: --pos; break; case SB_LINEDOWN: ++pos; break;
        case SB_PAGEUP: pos -= static_cast<int>(info.nPage); break; case SB_PAGEDOWN: pos += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: pos = info.nTrackPos; break; case SB_TOP: pos = 0; break; case SB_BOTTOM: pos = info.nMax; break;
        default: break;
        }
        scrollbars(); InvalidateRect(window_, nullptr, FALSE); return 0;
    }
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) && a == 'C') { copy(); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && a == 'A' && document_) { anchor_ = 0; caret_ = document_->text.size(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        if (a == VK_UP) scroll(-1, 0); else if (a == VK_DOWN) scroll(1, 0);
        else if (a == VK_LEFT) scroll(0, -4); else if (a == VK_RIGHT) scroll(0, 4);
        else if (a == VK_PRIOR) scroll(-visibleRows(), 0); else if (a == VK_NEXT) scroll(visibleRows(), 0);
        else if (a == VK_HOME) { top_ = left_ = 0; scroll(0, 0); }
        else if (a == VK_END && document_) { top_ = static_cast<int>(document_->lines.size()); scroll(0, 0); }
        return 0;
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING | (anchor_ == caret_ ? MF_GRAYED : 0), 1, L"Copy\tCtrl+C"); AppendMenuW(menu, MF_STRING, 2, L"Select all\tCtrl+A");
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b)}; if (p.x == -1) { p = {gutter_, header_}; ClientToScreen(window_, &p); }
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window_, nullptr); DestroyMenu(menu);
        if (command == 1) copy(); else if (command == 2 && document_) { anchor_ = 0; caret_ = document_->text.size(); InvalidateRect(window_, nullptr, FALSE); } return 0;
    }
    default: return DefWindowProcW(window_, m, a, b);
    }
}
}
