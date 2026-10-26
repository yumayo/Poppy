#include "viewer.h"
#include <algorithm>
#include <array>
#include <imm.h>

namespace poppy {
namespace {
constexpr std::array<Color, 6> styles{foreground, rgb(191,151,220), rgb(151,195,121), rgb(107,129,145), rgb(224,192,128), rgb(231,112,118)};
constexpr Color removedBackground = rgb(62, 39, 45), addedBackground = rgb(36, 57, 44);
}
void Viewer::create(HWND parent, HINSTANCE instance) {
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyViewer"; cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Code viewer", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, instance, this);
    setDpi(GetDpiForWindow(parent));
}
void Viewer::setDpi(UINT dpi) {
    renderer_.setDpi(dpi);
    cellWidth_ = renderer_.cellWidth(); cellHeight_ = renderer_.textHeight() + MulDiv(3, dpi, 96);
    header_ = MulDiv(34, dpi, 96); gutter_ = MulDiv(70, dpi, 96);
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::show(std::shared_ptr<const Document> d, const ResourceId& path, bool preserve) {
    edit_.reset(); highSurrogate_ = 0;
    diff_.reset(); document_ = std::move(d); path_ = path; message_.clear(); title_.clear();
    if (!preserve) { top_ = left_ = 0; anchor_ = caret_ = 0; after_ = false; }
    else { anchor_ = std::min(anchor_, document_->text.size()); caret_ = std::min(caret_, document_->text.size()); }
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::edit(std::shared_ptr<EditBuffer> buffer, const ResourceId& path, bool preserve) {
    show(std::shared_ptr<const Document>(buffer, &buffer->document), path, preserve);
    edit_ = std::move(buffer); caretPosition();
}
void Viewer::showDiff(std::shared_ptr<const DiffDocument> d, const ResourceId& path, bool preserve, std::wstring title) {
    edit_.reset(); highSurrogate_ = 0;
    // Share the patch snapshot; both panes only keep references to its lines.
    document_ = std::shared_ptr<const Document>(d, &d->patch); diff_ = std::move(d); path_ = path; message_.clear(); title_ = std::move(title);
    if (!preserve) { top_ = left_ = 0; anchor_ = caret_ = 0; after_ = false; }
    else { anchor_ = std::min(anchor_, document_->text.size()); caret_ = std::min(caret_, document_->text.size()); }
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::message(const std::wstring& text, const ResourceId& path) {
    edit_.reset(); highSurrogate_ = 0;
    diff_.reset(); document_.reset(); path_ = path; title_.clear(); message_ = text; top_ = left_ = 0; anchor_ = caret_ = 0; after_ = false;
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
Viewer::State Viewer::state() const {
    // Inline row indices retain both sides without storing a layout per tab.
    const int top = diff_ ? static_cast<int>(diff_->mapRow(static_cast<size_t>(top_), inlineView_, true, after_)) : top_;
    return {top, left_, anchor_, caret_, after_};
}
void Viewer::restore(const State& state) {
    if (!document_) return;
    top_ = diff_ ? static_cast<int>(diff_->mapRow(static_cast<size_t>(std::max(0, state.top)), true, inlineView_, state.after)) : state.top;
    left_ = state.left; after_ = state.after;
    anchor_ = std::min(state.anchor, document_->text.size()); caret_ = std::min(state.caret, document_->text.size());
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::toggleDiffLayout() { if (diff_) setDiffLayout(!inlineView_); }
void Viewer::setDiffLayout(bool inlineView) {
    if (inlineView_ == inlineView) return;
    // Keep the source line at the top when the two layouts have different row counts.
    if (diff_) top_ = static_cast<int>(diff_->mapRow(static_cast<size_t>(top_), inlineView_, inlineView, after_));
    inlineView_ = inlineView;
    if (selecting_) { selecting_ = false; ReleaseCapture(); }
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
    if (diffLayoutChanged) diffLayoutChanged();
}
RECT Viewer::layoutButton(bool inlineView) const {
    const auto rect = Scrollbars::contentRect(window_);
    const int width = header_ * 2, right = static_cast<int>(rect.right) - header_ / 3;
    return {right - width * (inlineView ? 1 : 2), header_ / 6,
        right - (inlineView ? 0 : width), header_ - header_ / 6};
}
int Viewer::visibleRows() const { const auto r = Scrollbars::contentRect(window_); return std::max(1, (static_cast<int>(r.bottom) - contentTop()) / cellHeight_); }
void Viewer::scrollbars() {
    if (!window_) return;
    const int rows = document_ ? std::max(1, static_cast<int>(rowCount())) : 1;
    const int cols = document_ ? static_cast<int>(diff_ ? diff_->maxColumns : document_->maxColumns) : 1;
    // Each bar can change the space available to the other.
    for (int pass = 0; pass < 3; ++pass) {
        const auto r = Scrollbars::contentRect(window_);
        const int width = splitDiff() ? (static_cast<int>(r.right) - 1) / 2 : static_cast<int>(r.right);
        const int pageRows = visibleRows(), pageCols = std::max(1, (width - textGutter()) / cellWidth_);
        top_ = std::clamp(top_, 0, std::max(0, rows - pageRows)); left_ = std::clamp(left_, 0, std::max(0, cols - pageCols));
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0, rows - 1, static_cast<UINT>(pageRows), top_, 0}; SetScrollInfo(window_, SB_VERT, &vertical, FALSE);
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0, cols - 1, static_cast<UINT>(pageCols), left_, 0}; SetScrollInfo(window_, SB_HORZ, &horizontal, FALSE);
    }
}
void Viewer::scroll(int vertical, int horizontal) {
    top_ += vertical; left_ += horizontal; scrollbars(); caretPosition();
    InvalidateRect(window_, nullptr, FALSE);
    // WM_PAINT has lower priority than mouse input. Present this position
    // before handling another move, including while a thumb is dragged quickly.
    UpdateWindow(window_);
}
size_t Viewer::position(int x, int y) const {
    if (!document_ || !rowCount()) return 0;
    size_t row = std::min(rowCount() - 1, static_cast<size_t>(std::max(0, (y - contentTop()) / cellHeight_ + top_)));
    int origin = textGutter();
    auto line = document_->line(row); size_t offset = document_->lines[row];
    if (diff_) {
        const auto rect = Scrollbars::contentRect(window_);
        if (splitDiff()) {
            origin += after_ ? static_cast<int>(rect.right) / 2 + 1 : 0;
            x = std::min(x, static_cast<int>(after_ ? rect.right : rect.right / 2));
        }
        auto change = diff_->displayRow(row, inlineView_);
        auto source = (inlineView_ ? change.after.number != 0 : after_) ? change.after : change.before;
        if (!source.number) {
            // Empty alignment cells map to the next source line's start, or
            // the preceding line's end when this side has no more content.
            for (size_t next = row + 1; next < rowCount(); ++next) {
                change = diff_->displayRow(next, inlineView_); source = after_ ? change.after : change.before;
                if (source.number) return diff_->offset(source);
            }
            for (size_t previous = row; previous > 0; --previous) {
                change = diff_->displayRow(previous - 1, inlineView_); source = after_ ? change.after : change.before;
                if (source.number) return diff_->offset(source) + diff_->line(source).size();
            }
            return 0;
        }
        line = diff_->line(source); offset = diff_->offset(source);
    }
    const int target = std::max(0, (x - origin + cellWidth_ / 2) / cellWidth_ + left_);
    int col = 0; size_t p = 0;
    while (p < line.size() && col < target) {
        const size_t before = p; const char32_t c = decodeUtf8(line, p);
        const int width = c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c));
        if (col + width > target) { p = before; break; } col += width;
    }
    return offset + p;
}
void Viewer::copy() {
    if (!document_ || anchor_ == caret_) return;
    const size_t start = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
    if (diff_) copyText(window_, wide(diff_->selectedText(after_, start, end, inlineView_)));
    else copyText(window_, wide(std::string_view(document_->text).substr(start, end - start)));
}
POINT Viewer::caretPoint() const {
    if (!edit_) return {gutter_, contentTop()};
    const auto& d = edit_->document;
    const size_t row = static_cast<size_t>(std::upper_bound(d.lines.begin(), d.lines.end(), caret_) - d.lines.begin() - 1);
    const auto line = d.line(row); int col = 0;
    for (size_t p = 0; p < line.size() && d.lines[row] + p < caret_;) { const auto c = decodeUtf8(line, p); col += c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c)); }
    return {gutter_ + (col - left_) * cellWidth_, contentTop() + (static_cast<int>(row) - top_) * cellHeight_};
}
void Viewer::caretPosition(bool reveal) {
    if (!edit_) {
        KillTimer(window_, caretTimer); caretVisible_ = false;
        if (GetFocus() == window_) DestroyCaret();
        return;
    }
    auto point = caretPoint();
    if (reveal) {
        const auto r = Scrollbars::contentRect(window_);
        if (point.y < contentTop()) top_ -= (contentTop() - point.y + cellHeight_ - 1) / cellHeight_;
        else if (point.y + cellHeight_ > r.bottom) top_ += (point.y + cellHeight_ - r.bottom + cellHeight_ - 1) / cellHeight_;
        if (point.x < gutter_) left_ -= (gutter_ - point.x + cellWidth_ - 1) / cellWidth_;
        else if (point.x + cellWidth_ > r.right) left_ += (point.x + cellWidth_ - r.right + cellWidth_ - 1) / cellWidth_;
        scrollbars(); point = caretPoint();
    }
    if (GetFocus() == window_) {
        // Keep the native caret hidden for IME/accessibility. Its XOR drawing
        // races with Direct2D presentation and can leave marks after a move.
        DestroyCaret(); CreateCaret(window_, nullptr, 2, cellHeight_); SetCaretPos(point.x, point.y);
        caretVisible_ = true; KillTimer(window_, caretTimer);
        const UINT blink = GetCaretBlinkTime();
        if (blink != INFINITE) SetTimer(window_, caretTimer, blink ? blink : 500, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
        const auto context = ImmGetContext(window_);
        if (context) { COMPOSITIONFORM form{}; form.dwStyle = CFS_POINT; form.ptCurrentPos = point; ImmSetCompositionWindow(context, &form); CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS; candidate.ptCurrentPos = {point.x, point.y + cellHeight_}; ImmSetCandidateWindow(context, &candidate); ImmReleaseContext(window_, context); }
    }
}
void Viewer::edited() { scrollbars(); caretPosition(true); InvalidateRect(window_, nullptr, FALSE); if (changed) changed(); }
void Viewer::replace(std::string text) {
    if (!edit_) return;
    std::string normalized; const auto newline = edit_->newline();
    for (size_t p = 0; p < text.size(); ++p) {
        if (text[p] == '\r' || text[p] == '\n') { if (text[p] == '\r' && p + 1 < text.size() && text[p + 1] == '\n') ++p; normalized += newline; }
        else normalized += text[p];
    }
    const size_t begin = std::min(anchor_, caret_), end = std::max(anchor_, caret_), length = normalized.size();
    if (edit_->replace(begin, end, std::move(normalized))) { anchor_ = caret_ = begin + length; edited(); }
    else MessageBeep(MB_ICONWARNING);
}
void Viewer::moveCaret(WPARAM key, bool shift, bool ctrl) {
    const auto& d = edit_->document;
    const size_t row = static_cast<size_t>(std::upper_bound(d.lines.begin(), d.lines.end(), caret_) - d.lines.begin() - 1);
    size_t next = caret_;
    if (key == VK_LEFT) next = !shift && anchor_ != caret_ ? std::min(anchor_, caret_) : edit_->previous(caret_);
    else if (key == VK_RIGHT) next = !shift && anchor_ != caret_ ? std::max(anchor_, caret_) : edit_->next(caret_);
    else if (key == VK_HOME) next = ctrl ? 0 : d.lines[row];
    else if (key == VK_END) next = ctrl ? d.text.size() : d.lines[row] + d.line(row).size();
    else {
        const int delta = key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -visibleRows() : visibleRows();
        const auto target = static_cast<size_t>(std::clamp<int>(static_cast<int>(row) + delta, 0, static_cast<int>(d.lines.size()) - 1));
        const auto point = caretPoint(); next = position(point.x, contentTop() + (static_cast<int>(target) - top_) * cellHeight_);
    }
    caret_ = next; if (!shift) anchor_ = caret_; caretPosition(true); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    auto& dc = renderer_; dc.font(Renderer::Font::Ui);
    const auto r = Scrollbars::contentRect(window_);
    RECT header{0, 0, r.right, header_}; dc.fill(header, panel);
    const LONG titleRight = diff_ ? layoutButton(false).left - 14 : r.right - 175;
    RECT title{14, 0, std::max<LONG>(14, titleRight), header_};
    dc.label(title, !title_.empty() ? title_ : path_.empty() ? L"POPPY" : path_.filename().native(), foreground);
    RECT tag{std::max<LONG>(14, r.right - 175), 0, r.right - 12, header_};
    if (diff_) {
        for (const bool inlineView : {false, true}) {
            const auto button = layoutButton(inlineView);
            dc.fill(button, inlineView == inlineView_ ? rgb(50, 70, 94) : background);
            dc.label(button, inlineView ? L"Inline" : L"Split", inlineView == inlineView_ ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else dc.label(tag, document_ ? wide(document_->encoding) + (edit_ ? edit_->dirty() ? L"  ·  Modified" : L"  ·  Ctrl+S" : L"  ·  READ ONLY") : L"", muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    dc.fill({0, header_ - 1, r.right, header_}, border);
    if (!document_) {
        dc.fill({0, header_, r.right, r.bottom}, background);
        RECT text{32, header_ + 48, std::max<LONG>(32, r.right - 32), r.bottom};
        dc.label(text, message_, muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        const int body = contentTop(), middle = static_cast<int>(r.right) / 2;
        if (diff_) {
            dc.fill({0, header_, r.right, body}, panel);
            if (inlineView_) {
                dc.label({0, header_, gutter_ - 14, body}, L"Before", muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                dc.label({gutter_, header_, 2 * gutter_ - 14, body}, L"After", muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            } else {
                dc.label({14, header_, middle, body}, L"Before", muted);
                dc.label({middle + 14, header_, r.right, body}, L"After", muted);
            }
            dc.fill({0, body - 1, r.right, body}, border);
        }
        dc.font(Renderer::Font::Code);
        const int rows = std::max(0, (static_cast<int>(r.bottom) - body + cellHeight_ - 1) / cellHeight_);
        const size_t selectionStart = std::min(anchor_, caret_), selectionEnd = std::max(anchor_, caret_);
        std::vector<Token> tokens;
        for (int side = 0; side < (splitDiff() ? 2 : 1); ++side) {
            const int paneLeft = side ? middle + 1 : 0, paneRight = splitDiff() && !side ? middle : static_cast<int>(r.right);
            const int textLeft = paneLeft + textGutter();
            const int columns = std::max(0, (paneRight - textLeft) / cellWidth_ + 1);
            std::vector<Cell> cells(static_cast<size_t>(columns));
            for (int screenRow = 0; screenRow < rows; ++screenRow) {
                const int y = body + screenRow * cellHeight_; const size_t row = static_cast<size_t>(top_ + screenRow);
                Color rowBackground = background; DiffLine source; DiffRow change;
                if (diff_ && row < rowCount()) {
                    change = diff_->displayRow(row, inlineView_);
                    const bool after = inlineView_ ? change.after.number != 0 : side != 0;
                    source = after ? change.after : change.before;
                    if (source.number && change.changed) rowBackground = after ? addedBackground : removedBackground;
                    else if (!source.number) rowBackground = panel;
                }
                dc.fill({paneLeft, y, paneRight, y + cellHeight_}, rowBackground);
                if (change.heading) continue;
                if (row >= rowCount() || (diff_ && !source.number)) continue;
                if (diff_ && inlineView_) {
                    if (change.before.number) dc.label({0, y, std::min(paneRight, gutter_ - 14), y + cellHeight_}, std::to_wstring(change.before.number), muted, DT_RIGHT | DT_SINGLELINE);
                    if (change.after.number) dc.label({gutter_, y, std::min(paneRight, textLeft - 14), y + cellHeight_}, std::to_wstring(change.after.number), muted, DT_RIGHT | DT_SINGLELINE);
                } else {
                    RECT number{paneLeft, y, std::min(paneRight, textLeft - 14), y + cellHeight_};
                    dc.label(number, std::to_wstring(diff_ ? source.number : row + 1), muted, DT_RIGHT | DT_SINGLELINE);
                }
                auto line = diff_ ? diff_->line(source) : document_->line(row);
                const size_t lineOffset = diff_ ? diff_->offset(source) : document_->lines[row];
                line = line.substr(0, std::min(line.size(), maxLineColumns * 4));
                tokens.clear();
                if (!diff_) { LexState state = document_->states[row]; lexLine(line, document_->language, state, &tokens); }
                std::fill(cells.begin(), cells.end(), Cell{U' ', foreground, rowBackground, false});
                int col = 0; size_t token = 0;
                for (size_t p = 0; p < line.size() && col < left_ + columns && col < static_cast<int>(maxLineColumns);) {
                    const size_t begin = p; char32_t c = decodeUtf8(line, p);
                    const int width = c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c));
                    while (token < tokens.size() && tokens[token].end <= begin) ++token;
                    const auto style = token < tokens.size() && tokens[token].begin <= begin ? tokens[token].style : Style::Text;
                    const size_t offset = lineOffset + begin;
                    const Color bg = (!splitDiff() || static_cast<bool>(side) == after_) && offset >= selectionStart && offset < selectionEnd ? rgb(50, 70, 94) : rowBackground;
                    if (c < 32) c = U' ';
                    for (int i = 0; i < width; ++i) {
                        const int x = col + i - left_; if (x < 0 || x >= columns) continue;
                        const bool wideContinuation = cellWidth(c) == 2 && i == 1;
                        cells[x] = {i == 0 ? c : U' ', styles[static_cast<size_t>(style)], bg, wideContinuation};
                        if (i > 0 && col < left_) cells[x] = {U' ', foreground, bg, false};
                    }
                    col += width;
                }
                RECT clip{textLeft, body, paneRight, r.bottom}; dc.cells(textLeft, y, cellWidth_, cellHeight_, cells, columns, clip);
            }
        }
        if (splitDiff()) dc.fill({middle, body, middle + 1, r.bottom}, border);
        if (diff_) {
            dc.font(Renderer::Font::Ui);
            for (int screenRow = 0; screenRow < rows; ++screenRow) {
                const size_t row = static_cast<size_t>(top_ + screenRow);
                if (row >= rowCount()) break;
                const auto change = diff_->displayRow(row, inlineView_);
                if (!change.heading) continue;
                const int y = body + screenRow * cellHeight_;
                dc.fill({0, y, r.right, y + cellHeight_}, panel);
                auto text = document_->line(change.heading - 1);
                if (text.substr(0, 11) == "diff --git ") text.remove_prefix(11);
                dc.label({14, y, r.right - 12, y + cellHeight_}, wide(text), accent);
            }
        }
        if (edit_ && caretVisible_ && GetFocus() == window_) {
            const auto point = caretPoint();
            const RECT caret{point.x, point.y, point.x + 2, point.y + cellHeight_};
            const RECT bodyRect{gutter_, body, r.right, r.bottom};
            RECT clipped{};
            if (IntersectRect(&clipped, &caret, &bodyRect)) dc.fill(clipped, foreground);
        }
    }
    scrollbars_.paint(window_, dc, background);
}
LRESULT CALLBACK Viewer::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<Viewer*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT Viewer::handle(UINT m, WPARAM a, LPARAM b) {
    LRESULT result = 0;
    if (scrollbars_.handle(window_, m, a, b, result)) return result;
    switch (m) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: scrollbars(); caretPosition(); return 0;
    case WM_SETTINGCHANGE: caretPosition(); return 0;
    case WM_SETFOCUS: caretPosition(); if (focused) focused(); return 0;
    case WM_KILLFOCUS:
        highSurrogate_ = 0; caretVisible_ = false; KillTimer(window_, caretTimer); DestroyCaret();
        InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_TIMER:
        if (a == caretTimer && edit_ && GetFocus() == window_ && GetCaretBlinkTime() != INFINITE) {
            caretVisible_ = !caretVisible_; InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY: KillTimer(window_, caretTimer); return 0;
    case WM_SETCURSOR:
        if (diff_ && LOWORD(b) == HTCLIENT) {
            POINT point{}; GetCursorPos(&point); ScreenToClient(window_, &point);
            for (const bool inlineView : {false, true}) {
                const auto button = layoutButton(inlineView);
                if (PtInRect(&button, point)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            }
        }
        return DefWindowProcW(window_, m, a, b);
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        if (diff_) for (const bool inlineView : {false, true}) {
            const auto button = layoutButton(inlineView);
            if (PtInRect(&button, point)) { setDiffLayout(inlineView); return 0; }
        }
        if (point.y < contentTop()) return 0;
        bool after = splitDiff() && point.x >= Scrollbars::contentRect(window_).right / 2;
        if (diff_ && inlineView_ && rowCount()) {
            const size_t row = std::min(rowCount() - 1, static_cast<size_t>((point.y - contentTop()) / cellHeight_ + top_));
            after = diff_->displayRow(row, true).after.number != 0;
        }
        const bool extend = (GetKeyState(VK_SHIFT) & 0x8000) && (!splitDiff() || after == after_);
        after_ = after;
        if (!extend) anchor_ = position(GET_X_LPARAM(b), GET_Y_LPARAM(b));
        caret_ = position(GET_X_LPARAM(b), GET_Y_LPARAM(b)); selecting_ = true; SetCapture(window_); caretPosition(); InvalidateRect(window_, nullptr, FALSE); return 0;
    }
    case WM_MOUSEMOVE: if (selecting_) { caret_ = position(GET_X_LPARAM(b), GET_Y_LPARAM(b)); caretPosition(); InvalidateRect(window_, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: if (selecting_) { selecting_ = false; ReleaseCapture(); } return 0;
    case WM_CAPTURECHANGED: selecting_ = false; return 0;
    case WM_MOUSEWHEEL: scroll(-GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * 3, 0); return 0;
    case WM_VSCROLL: case WM_HSCROLL: {
        const bool vertical = m == WM_VSCROLL; int& pos = vertical ? top_ : left_;
        SCROLLINFO info{sizeof(info), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window_, vertical ? SB_VERT : SB_HORZ, &info);
        switch (LOWORD(a)) {
        case SB_LINEUP: --pos; break; case SB_LINEDOWN: ++pos; break;
        case SB_PAGEUP: pos -= static_cast<int>(info.nPage); break; case SB_PAGEDOWN: pos += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = scrollbars_.trackPosition(vertical ? SB_VERT : SB_HORZ, info.nTrackPos); break;
        case SB_TOP: pos = 0; break; case SB_BOTTOM: pos = info.nMax; break;
        default: return 0;
        }
        scroll(0, 0); return 0;
    }
    case WM_KEYDOWN:
        if (edit_) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && a == 'S') { if (save) save(); return 0; }
            if (ctrl && a == 'X') { copy(); replace(""); return 0; }
            if ((ctrl && a == 'V') || (shift && a == VK_INSERT)) { replace(utf8(clipboardText(window_))); return 0; }
            if (ctrl && (a == 'Z' || a == 'Y')) { if (a == 'Y' || shift) edit_->redo(anchor_, caret_); else edit_->undo(anchor_, caret_); edited(); return 0; }
            if (a == VK_DELETE || a == VK_BACK) {
                if (anchor_ == caret_) { if (a == VK_BACK) anchor_ = edit_->previous(caret_); else caret_ = edit_->next(caret_); }
                replace(""); return 0;
            }
            if (a == VK_TAB) { replace("\t"); return 0; }
            if (a == VK_LEFT || a == VK_RIGHT || a == VK_UP || a == VK_DOWN || a == VK_HOME || a == VK_END || a == VK_PRIOR || a == VK_NEXT) { moveCaret(a, shift, ctrl); return 0; }
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && a == 'C') { copy(); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && a == 'A' && document_) { anchor_ = 0; caret_ = document_->text.size(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        if (a == VK_UP) scroll(-1, 0); else if (a == VK_DOWN) scroll(1, 0);
        else if (a == VK_LEFT) scroll(0, -4); else if (a == VK_RIGHT) scroll(0, 4);
        else if (a == VK_PRIOR) scroll(-visibleRows(), 0); else if (a == VK_NEXT) scroll(visibleRows(), 0);
        else if (a == VK_HOME) { top_ = left_ = 0; scroll(0, 0); }
        else if (a == VK_END && document_) { top_ = static_cast<int>(rowCount()); scroll(0, 0); }
        return 0;
    case WM_CHAR:
        if (edit_ && !(GetKeyState(VK_CONTROL) & 0x8000) && (a >= 32 || a == '\r')) {
            const wchar_t c = static_cast<wchar_t>(a);
            if (c >= 0xd800 && c <= 0xdbff) { highSurrogate_ = c; return 0; }
            std::wstring text; if (highSurrogate_) { text += highSurrogate_; highSurrogate_ = 0; } text += c;
            replace(utf8(text));
        }
        return 0;
    case WM_IME_STARTCOMPOSITION: caretPosition(); break;
    case WM_IME_COMPOSITION:
        if (edit_ && (b & GCS_RESULTSTR)) {
            const auto context = ImmGetContext(window_); const LONG bytes = context ? ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0) : 0;
            if (bytes > 0) { std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), 0); ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes); replace(utf8(text)); }
            if (context) ImmReleaseContext(window_, context);
            return 0;
        }
        break;
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING | (anchor_ == caret_ ? MF_GRAYED : 0), 1, L"Copy\tCtrl+C"); AppendMenuW(menu, MF_STRING, 2, L"Select all\tCtrl+A");
        if (edit_) { AppendMenuW(menu, MF_STRING, 3, L"Cut\tCtrl+X"); AppendMenuW(menu, MF_STRING, 4, L"Paste\tCtrl+V"); AppendMenuW(menu, MF_STRING, 5, L"Undo\tCtrl+Z"); AppendMenuW(menu, MF_STRING, 6, L"Redo\tCtrl+Y"); AppendMenuW(menu, MF_STRING, 7, L"Save\tCtrl+S"); }
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b)}; if (p.x == -1) { p = {gutter_, header_}; ClientToScreen(window_, &p); }
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window_, nullptr); DestroyMenu(menu);
        if (command == 1) copy(); else if (command == 2 && document_) { anchor_ = 0; caret_ = document_->text.size(); InvalidateRect(window_, nullptr, FALSE); }
        else if (edit_) { if (command == 3) { copy(); replace(""); } else if (command == 4) replace(utf8(clipboardText(window_))); else if (command == 5) { edit_->undo(anchor_, caret_); edited(); } else if (command == 6) { edit_->redo(anchor_, caret_); edited(); } else if (command == 7 && save) save(); }
        return 0;
    }
    default: return DefWindowProcW(window_, m, a, b);
    }
    return DefWindowProcW(window_, m, a, b);
}
}

size_t poppy::Viewer::documentBytes() const {
    size_t bytes = document_ ? document_->text.capacity() + document_->lines.capacity() * sizeof(uint32_t) + document_->states.capacity() * sizeof(LexState) : 0;
    if (diff_) bytes += diff_->rows.capacity() * sizeof(DiffRow) + diff_->inlineOrder.capacity() * sizeof(uint32_t);
    return bytes;
}
