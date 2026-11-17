#include "csv_diff_view.h"
#include "change_marks.h"
#include "selection_scroll.h"
#include <algorithm>

namespace poppy {
namespace {
// Subtle tints: a changed row is barely tinted and a changed cell a little
// more, so the values stay easy to read. Grid lines are lighter than the
// tints, which keeps them visible on changed rows too.
constexpr COLORREF removedRow = RGB(36, 30, 36), addedRow = RGB(28, 37, 35);
constexpr COLORREF removedCell = RGB(66, 41, 48), addedCell = RGB(40, 64, 49);
constexpr COLORREF missingRow = RGB(26, 29, 35), gridLine = RGB(56, 64, 76);
// Drawn over the cell's own color, so a selected changed cell keeps its tint.
constexpr COLORREF selectedCell = RGB(70, 110, 160), activeHeader = RGB(43, 67, 52);
std::wstring displayValue(std::wstring text) {
    for (auto& ch : text) {
        if (ch == L'\r') ch = L'␍';
        else if (ch == L'\n') ch = L'↵';
        else if (ch == L'\t') ch = L'⇥';
    }
    return text;
}
}
void CsvDiffView::show(std::shared_ptr<const CsvComparison> comparison, bool preserve) {
    comparison_ = std::move(comparison); active_ = true;
    stopDrag();
    if (!preserve) { top_ = left_ = 0; row_ = column_ = anchorRow_ = anchorColumn_ = 0; after_ = true; }
    changes_.clear();
    for (size_t i = 0; i < rows(); ++i) {
        const auto& row = comparison_->rows[i];
        if (!row.changed) continue;
        const bool removed = row.after == CsvComparison::none;
        if (!changes_.empty() && changes_.back().end == i) { changes_.back().end = i + 1; changes_.back().removed = changes_.back().removed && removed; }
        else changes_.push_back({i, i + 1, removed});
    }
    row_ = std::min(row_, std::max<size_t>(1, rows()) - 1); column_ = std::min(column_, std::max<size_t>(1, columns()) - 1);
    anchorRow_ = std::min(anchorRow_, std::max<size_t>(1, rows()) - 1); anchorColumn_ = std::min(anchorColumn_, std::max<size_t>(1, columns()) - 1);
    layout(); InvalidateRect(window_, nullptr, FALSE);
    if (statusChanged) statusChanged();
}
void CsvDiffView::hide(bool release) {
    stopDrag(); active_ = false;
    if (release) { comparison_.reset(); changes_ = {}; }
}
int CsvDiffView::gutter() const {
    const size_t records = comparison_ ? std::max(comparison_->before.rows(), comparison_->after.rows()) : 0;
    return std::max(px(52), static_cast<int>(std::to_wstring(records).size()) * renderer_->cellWidth() + px(18));
}
RECT CsvDiffView::half(bool after) const {
    auto rect = Scrollbars::contentRect(window_);
    rect.right = std::max(0L, rect.right - px(4));
    const int middle = static_cast<int>(rect.right) / 2;
    if (after) rect.left = std::min<LONG>(rect.right, middle + px(2)); else rect.right = std::max(0, middle - px(2));
    return rect;
}
int CsvDiffView::pageRows() const { return std::max(1, static_cast<int>(Scrollbars::contentRect(window_).bottom - bodyTop()) / rowHeight()); }
void CsvDiffView::layout() {
    if (!active_ || !window_) return;
    for (int pass = 0; pass < 3; ++pass) {
        const auto area = half(false);
        const int count = static_cast<int>(rows()), width = static_cast<int>(columns()) * columnWidth();
        const int page = pageRows(), visible = std::max(1, static_cast<int>(area.right - area.left) - gutter());
        top_ = std::clamp(top_, 0, std::max(0, count - page)); left_ = std::clamp(left_, 0, std::max(0, width - visible));
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0, count - 1, static_cast<UINT>(page), top_, 0};
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0, width - 1, static_cast<UINT>(visible), left_, 0};
        SetScrollInfo(window_, SB_VERT, &vertical, FALSE); SetScrollInfo(window_, SB_HORZ, &horizontal, FALSE);
    }
}
bool CsvDiffView::hit(POINT point, size_t& row, size_t& column, bool& after) const {
    if (!rows() || point.y < bodyTop()) return false;
    for (const bool side : {false, true}) {
        const auto area = half(side);
        if (point.x < area.left + gutter() || point.x >= area.right) continue;
        const size_t r = static_cast<size_t>((point.y - bodyTop()) / rowHeight() + top_);
        const size_t c = static_cast<size_t>((point.x - area.left - gutter() + left_) / columnWidth());
        if (r >= rows() || c >= columns()) return false;
        row = r; column = c; after = side; return true;
    }
    return false;
}
void CsvDiffView::hitSide(POINT point, bool after, size_t& row, size_t& column) const {
    const auto area = half(after);
    const int y = std::max(0, static_cast<int>(point.y) - bodyTop()) / rowHeight() + top_;
    const int x = std::max(0, static_cast<int>(std::min<LONG>(point.x, area.right - 1)) - static_cast<int>(area.left) - gutter() + left_) / columnWidth();
    row = std::min(static_cast<size_t>(y), std::max<size_t>(1, rows()) - 1);
    column = std::min(static_cast<size_t>(x), std::max<size_t>(1, columns()) - 1);
}
CsvDiffView::Range CsvDiffView::selection() const {
    return {std::min(anchorRow_, row_), std::min(anchorColumn_, column_), std::max(anchorRow_, row_), std::max(anchorColumn_, column_)};
}
void CsvDiffView::select(size_t row, size_t column, bool extend) {
    row_ = std::min(row, std::max<size_t>(1, rows()) - 1); column_ = std::min(column, std::max<size_t>(1, columns()) - 1);
    if (!extend) { anchorRow_ = row_; anchorColumn_ = column_; }
    reveal(); InvalidateRect(window_, nullptr, FALSE);
    if (statusChanged) statusChanged();
}
void CsvDiffView::drag(POINT point) {
    dragPoint_ = point;
    size_t row = 0, column = 0; hitSide(point, after_, row, column);
    if (dragHeader_ == 1) { anchorColumn_ = 0; column = columns() - 1; }
    if (dragHeader_ == 2) { anchorRow_ = 0; row = rows() - 1; }
    if (row == row_ && column == column_) return;
    // The pointer's cell is already on screen, or the timer scrolls to it.
    row_ = row; column_ = column;
    InvalidateRect(window_, nullptr, FALSE);
    if (statusChanged) statusChanged();
}
void CsvDiffView::stopDrag() {
    if (!selecting_) return;
    selecting_ = false; KillTimer(window_, dragTimer);
    if (GetCapture() == window_) ReleaseCapture();
}
void CsvDiffView::reveal() {
    const int page = pageRows();
    if (static_cast<int>(row_) < top_) top_ = static_cast<int>(row_);
    else if (static_cast<int>(row_) >= top_ + page) top_ = static_cast<int>(row_) - page + 1;
    const auto area = half(false);
    const int visible = std::max(1, static_cast<int>(area.right - area.left) - gutter());
    const int left = static_cast<int>(column_) * columnWidth(), right = left + columnWidth();
    if (left < left_ || right - left > visible) left_ = left;
    else if (right > left_ + visible) left_ = right - visible;
    layout();
}
void CsvDiffView::move(WPARAM key, bool shift, bool ctrl) {
    size_t row = row_, column = column_;
    const size_t page = static_cast<size_t>(pageRows()), lastRow = std::max<size_t>(1, rows()) - 1, lastColumn = std::max<size_t>(1, columns()) - 1;
    switch (key) {
    case VK_UP: row = ctrl ? 0 : row - std::min<size_t>(row, 1); break;
    case VK_DOWN: row = ctrl ? lastRow : std::min(lastRow, row + 1); break;
    case VK_PRIOR: row -= std::min(row, page); break;
    case VK_NEXT: row = std::min(lastRow, row + page); break;
    case VK_LEFT: column = ctrl ? 0 : column - std::min<size_t>(column, 1); break;
    case VK_RIGHT: column = ctrl ? lastColumn : std::min(lastColumn, column + 1); break;
    case VK_HOME: column = 0; if (ctrl) row = 0; break;
    case VK_END: column = lastColumn; if (ctrl) row = lastRow; break;
    case VK_TAB:
        if (shift) { if (column) --column; else if (row) { --row; column = lastColumn; } }
        else if (column < lastColumn) ++column; else if (row < lastRow) { ++row; column = 0; }
        break;
    }
    select(row, column, shift && key != VK_TAB);
}
void CsvDiffView::nextChange(bool backwards) {
    if (changes_.empty()) { MessageBeep(MB_ICONWARNING); return; }
    const Change* target = nullptr;
    if (backwards) {
        for (auto it = changes_.rbegin(); it != changes_.rend() && !target; ++it) if (it->end <= row_) target = &*it;
        if (!target) target = &changes_.back();
    } else {
        for (const auto& change : changes_) if (change.begin > row_) { target = &change; break; }
        if (!target) target = &changes_.front();
    }
    // Show the change's first row near the top with a little context above it.
    top_ = static_cast<int>(target->begin) - std::min(3, pageRows() / 4);
    select(target->begin, column_);
}
void CsvDiffView::copy() {
    if (!rows()) return;
    // Like the CSV table, copy Excel-compatible TSV from the side the range
    // was selected on. Rows that side does not have are left out.
    const auto range = selection();
    if ((range.lastRow - range.firstRow + 1) > CsvTable::maxCells / (range.lastColumn - range.firstColumn + 1)) { MessageBeep(MB_ICONWARNING); return; }
    const bool single = range.firstColumn == range.lastColumn;
    std::string text; bool any = false;
    for (size_t r = range.firstRow; r <= range.lastRow; ++r) {
        const auto& row = comparison_->rows[r];
        if ((after_ ? row.after : row.before) == CsvComparison::none) continue;
        if (any) text += "\r\n";
        any = true;
        for (size_t c = range.firstColumn; c <= range.lastColumn; ++c) {
            if (c != range.firstColumn) text += '\t';
            const auto value = comparison_->value(after_, r, c);
            text += value.empty() && single ? "\"\"" : csvQuote(value, '\t');
            if (text.size() > maxFileBytes) { MessageBeep(MB_ICONWARNING); return; }
        }
    }
    if (!any || !copyText(window_, wide(text))) MessageBeep(MB_ICONWARNING);
}
std::wstring CsvDiffView::status() const {
    if (!comparison_) return {};
    if (!comparison_->valid()) return L"CSV table unavailable";
    size_t changed = 0;
    for (const auto& row : comparison_->rows) changed += row.changed;
    std::wstring selected;
    const auto range = selection();
    if (rows() && (range.firstRow != range.lastRow || range.firstColumn != range.lastColumn))
        selected = std::to_wstring(range.lastRow - range.firstRow + 1) + L" × " + std::to_wstring(range.lastColumn - range.firstColumn + 1) + L" selected  ·  ";
    return std::to_wstring(changed) + (changed == 1 ? L" changed row" : L" changed rows") + L"  ·  " + selected + L"F8: next change  ·  READ ONLY";
}
void CsvDiffView::paint() {
    Renderer::Paint paint(*renderer_, window_); if (!paint) return;
    auto& dc = *renderer_; dc.font(Renderer::Font::Ui);
    const auto rect = Scrollbars::contentRect(window_);
    dc.fill(rect, background);
    if (!comparison_ || !comparison_->valid()) {
        const auto error = comparison_ ? wide(comparison_->error) : std::wstring{};
        dc.label({px(24), insetTop() + px(24), rect.right - px(24), rect.bottom}, L"CSV table unavailable\n\n" + error + L"\n\nSwitch to Split or Inline to view this diff.", muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        scrollbars_->paint(window_, dc, background); return;
    }
    drawSide(false); drawSide(true);
    const auto left = half(false), right = half(true);
    dc.fill({left.right, insetTop(), right.left, rect.bottom}, border);
    drawMarks();
    scrollbars_->paint(window_, dc, background);
}
void CsvDiffView::drawSide(bool after) {
    auto& dc = *renderer_;
    const auto area = half(after); if (area.right <= area.left) return;
    const int rh = rowHeight(), cw = columnWidth(), g = std::min<int>(gutter(), area.right - area.left);
    const int origin = static_cast<int>(area.left) + g - left_;
    const size_t firstColumn = static_cast<size_t>(left_ / cw);
    const size_t lastColumn = std::min(columns(), static_cast<size_t>((left_ + area.right - area.left - g) / cw + 1));
    const size_t lastRow = std::min(rows(), static_cast<size_t>(top_ + std::max(0, static_cast<int>(area.bottom) - bodyTop() + rh - 1) / rh));
    const auto& side = after ? comparison_->after : comparison_->before;
    const RECT body{area.left + g, bodyTop(), area.right, area.bottom};
    const COLORREF changedRow = after ? addedRow : removedRow, changedCell = after ? addedCell : removedCell;
    const auto range = selection();
    for (size_t r = static_cast<size_t>(top_); r < lastRow; ++r) {
        const auto& row = comparison_->rows[r];
        const auto record = after ? row.after : row.before;
        const int y = bodyTop() + (static_cast<int>(r) - top_) * rh;
        const RECT line{body.left, y, body.right, std::min<LONG>(y + rh, body.bottom)};
        if (record == CsvComparison::none) { dc.fill(line, missingRow); dc.fill({line.left, line.bottom - 1, line.right, line.bottom}, gridLine); }
        else for (size_t c = firstColumn; c < lastColumn; ++c) {
            const RECT cell{origin + static_cast<int>(c) * cw, y, origin + static_cast<int>(c + 1) * cw, y + rh};
            RECT visible{}; if (!IntersectRect(&visible, &cell, &body)) continue;
            // A field this side does not have stays empty, like a missing row.
            const bool present = c < side.table.fields(record);
            dc.fill(visible, !present && row.changed ? missingRow : !row.changed ? background : comparison_->cellChanged(r, c) ? changedCell : changedRow);
            dc.fill({visible.left, visible.bottom - 1, visible.right, visible.bottom}, gridLine);
            if (cell.right <= body.right) dc.fill({cell.right - 1, visible.top, cell.right, visible.bottom}, gridLine);
            if (r >= range.firstRow && r <= range.lastRow && c >= range.firstColumn && c <= range.lastColumn)
                dc.fill({visible.left, visible.top, visible.right - 1, visible.bottom - 1}, selectedCell, after == after_ ? 0.35f : 0.18f);
            dc.label({cell.left + px(7), cell.top, cell.right - px(6), cell.bottom}, visible, displayValue(wide(side.table.value(side.text, {record, c}))), foreground);
        }
        const RECT number{area.left, y, area.left + g, std::min<LONG>(y + rh, area.bottom)};
        dc.fill(number, r >= range.firstRow && r <= range.lastRow ? activeHeader : row.changed && record != CsvComparison::none ? changedRow : panel);
        if (record != CsvComparison::none)
            dc.label({area.left, y, area.left + g, y + rh}, number, std::to_wstring(record + 1), r == row_ ? accent : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        dc.fill({number.left, number.bottom - 1, number.right, number.bottom}, border);
    }
    // The selected cell's frame, on both sides.
    if (row_ < rows() && column_ < columns()) {
        const int y = bodyTop() + (static_cast<int>(row_) - top_) * rh;
        const RECT frame{origin + static_cast<int>(column_) * cw - 1, y - 1, origin + static_cast<int>(column_ + 1) * cw, y + rh};
        const int thick = std::max(1, px(2));
        for (const RECT& strip : {RECT{frame.left, frame.top, frame.right, frame.top + thick}, RECT{frame.left, frame.bottom - thick, frame.right, frame.bottom},
                                  RECT{frame.left, frame.top, frame.left + thick, frame.bottom}, RECT{frame.right - thick, frame.top, frame.right, frame.bottom}}) {
            RECT visible{}; if (IntersectRect(&visible, &strip, &body)) dc.fill(visible, after == after_ ? accent : muted);
        }
    }
    // Column headers.
    const RECT header{area.left + g, headerTop(), area.right, std::min<LONG>(bodyTop(), area.bottom)};
    dc.fill({area.left, headerTop(), area.right, header.bottom}, panel);
    for (size_t c = firstColumn; c < lastColumn; ++c) {
        const RECT cell{origin + static_cast<int>(c) * cw, headerTop(), origin + static_cast<int>(c + 1) * cw, bodyTop()};
        RECT visible{}; if (!IntersectRect(&visible, &cell, &header)) continue;
        if (c >= range.firstColumn && c <= range.lastColumn) dc.fill(visible, activeHeader);
        dc.label(cell, visible, wide(csvColumnName(c)), c == column_ ? accent : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        dc.fill({visible.right - 1, visible.top, visible.right, visible.bottom}, border);
    }
    dc.fill({area.left, bodyTop() - 1, area.right, bodyTop()}, border);
    dc.fill({area.left + g - 1, headerTop(), area.left + g, area.bottom}, border);
    // The selected cell's name and value on this side.
    const RECT bar{area.left, insetTop(), area.right, std::min<LONG>(headerTop(), area.bottom)};
    dc.fill(bar, panel);
    std::wstring name = after ? L"After" : L"Before", value;
    if (row_ < rows()) {
        const auto record = after ? comparison_->rows[row_].after : comparison_->rows[row_].before;
        if (record != CsvComparison::none) { name += L"  " + wide(csvColumnName(column_)) + std::to_wstring(record + 1); value = displayValue(wide(comparison_->value(after, row_, column_))); }
    }
    const int nameWidth = std::min<int>(renderer_->textWidth(name) + px(20), area.right - area.left);
    dc.label({bar.left + px(10), bar.top, bar.left + nameWidth, bar.bottom}, name, after == after_ ? accent : muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const RECT box{bar.left + nameWidth, bar.top + px(4), std::max<LONG>(bar.left + nameWidth, bar.right - px(6)), bar.top + px(30)};
    if (box.right > box.left) {
        dc.fill(box, background); dc.outline(box, border);
        dc.label({box.left + px(6), box.top, box.right - px(4), box.bottom}, value, foreground);
    }
}
void CsvDiffView::drawMarks() {
    auto& dc = *renderer_;
    const auto r = Scrollbars::contentRect(window_);
    const int left = static_cast<int>(r.right) - px(4);
    dc.fill({left, insetTop(), r.right, r.bottom}, background);
    auto track = Scrollbars::trackRect(window_, SB_VERT);
    if (!(GetWindowLongPtrW(window_, GWL_STYLE) & WS_VSCROLL)) track = {left, bodyTop(), r.right, r.bottom};
    const size_t first = static_cast<size_t>(top_), last = first + static_cast<size_t>(pageRows());
    const size_t total = std::max(rows(), last);
    const auto thumb = Scrollbars::thumbRect(window_, SB_VERT);
    ChangeMarkVisibility visibility{first, last, thumb.top, thumb.bottom};
    const ChangeMarkViewport viewport{first, last, static_cast<int>(thumb.bottom - thumb.top)};
    for (const auto& change : changes_) {
        visibility.include(change.begin, change.end);
        const auto mark = changeMark(change.begin, change.end, total, track.top, track.bottom, std::max(1, px(3)), viewport);
        dc.fill(static_cast<float>(left), static_cast<float>(mark.top), static_cast<float>(r.right), static_cast<float>(mark.bottom),
            change.removed ? rgb(231, 112, 118) : rgb(151, 195, 121));
    }
    const auto mask = visibility.mask();
    dc.fill(static_cast<float>(left), static_cast<float>(mask.top), static_cast<float>(r.right), static_cast<float>(mask.bottom), background);
}
bool CsvDiffView::handle(UINT message, WPARAM a, LPARAM b, LRESULT& result) {
    result = 0;
    switch (message) {
    case WM_PAINT: paint(); return true;
    case WM_SIZE: layout(); InvalidateRect(window_, nullptr, FALSE); return true;
    case WM_GETDLGCODE: result = DLGC_WANTALLKEYS; return true;
    case WM_DESTROY: hide(true); return false;
    case WM_SETCURSOR:
        if (LOWORD(b) != HTCLIENT) return false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW)); result = TRUE; return true;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        SetFocus(window_); stopDrag();
        if (!rows()) return true;
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        const bool shift = (a & MK_SHIFT) != 0;
        size_t row = 0, column = 0; bool after = true;
        if (hit(point, row, column, after)) { if (!shift) after_ = after; dragHeader_ = 0; select(row, column, shift); }
        else {
            // Row numbers select whole rows, column names whole columns, and
            // their corner the whole table.
            int side = -1;
            for (const bool s : {false, true}) { const auto area = half(s); if (point.x >= area.left && point.x < area.right) side = s; }
            if (side < 0 || point.y < headerTop()) return true;
            after_ = side == 1;
            const auto area = half(after_);
            const bool gutterHit = point.x < area.left + gutter(), headerHit = point.y < bodyTop();
            if (gutterHit && headerHit) { anchorRow_ = anchorColumn_ = 0; row_ = rows() - 1; column_ = columns() - 1; InvalidateRect(window_, nullptr, FALSE); if (statusChanged) statusChanged(); return true; }
            hitSide({gutterHit ? static_cast<LONG>(area.left + gutter()) : point.x, headerHit ? static_cast<LONG>(bodyTop()) : point.y}, after_, row, column);
            dragHeader_ = gutterHit ? 1 : 2;
            if (!shift) { anchorRow_ = row; anchorColumn_ = column; }
            row_ = row; column_ = column;
            if (dragHeader_ == 1) { anchorColumn_ = 0; column_ = columns() - 1; }
            else { anchorRow_ = 0; row_ = rows() - 1; }
            InvalidateRect(window_, nullptr, FALSE); if (statusChanged) statusChanged();
        }
        selecting_ = true; dragPoint_ = point; SetCapture(window_); SetTimer(window_, dragTimer, 50, nullptr);
        return true;
    }
    case WM_RBUTTONDOWN: {
        // A right click inside the range keeps it for the context menu.
        SetFocus(window_);
        size_t row = 0, column = 0; bool after = true;
        if (hit({GET_X_LPARAM(b), GET_Y_LPARAM(b)}, row, column, after)) {
            const auto range = selection();
            const bool inside = row >= range.firstRow && row <= range.lastRow && column >= range.firstColumn && column <= range.lastColumn;
            after_ = after;
            if (!inside) select(row, column); else InvalidateRect(window_, nullptr, FALSE);
        }
        return true;
    }
    case WM_MOUSEMOVE:
        if (!selecting_) return false;
        drag({GET_X_LPARAM(b), GET_Y_LPARAM(b)}); return true;
    case WM_LBUTTONUP:
        if (!selecting_) return false;
        drag({GET_X_LPARAM(b), GET_Y_LPARAM(b)}); stopDrag(); return true;
    case WM_CANCELMODE: case WM_CAPTURECHANGED:
        if (selecting_ && (message == WM_CANCELMODE || reinterpret_cast<HWND>(b) != window_)) { selecting_ = false; KillTimer(window_, dragTimer); }
        return false;
    case WM_TIMER: {
        if (a != dragTimer) return false;
        if (!selecting_) { KillTimer(window_, dragTimer); return true; }
        // Scroll while the pointer is outside the body, then select through
        // the cell that scrolled under it.
        const auto area = half(after_); const auto bounds = Scrollbars::contentRect(window_);
        const int vertical = dragHeader_ != 2 ? selectionScrollRows(dragPoint_.y, bodyTop(), bounds.bottom, rowHeight()) : 0;
        const int horizontal = dragHeader_ != 1 ? selectionScrollRows(dragPoint_.x, area.left + gutter(), area.right, px(24)) * px(24) : 0;
        if (!vertical && !horizontal) return true;
        const int top = top_, left = left_;
        top_ += vertical; left_ += horizontal; layout();
        if (top != top_ || left != left_) {
            const auto point = dragPoint_;
            dragPoint_ = {std::clamp<LONG>(point.x, area.left + gutter(), std::max<LONG>(area.left + gutter(), area.right - 1)),
                std::clamp<LONG>(point.y, bodyTop(), std::max<LONG>(bodyTop(), bounds.bottom - 1))};
            drag(dragPoint_); dragPoint_ = point;
            InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_);
        }
        return true;
    }
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING | (rows() ? 0 : MF_GRAYED), 1, L"Copy\tCtrl+C");
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b)}; if (p.x == -1) { p = {px(24), bodyTop()}; ClientToScreen(window_, &p); }
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window_, nullptr); DestroyMenu(menu);
        if (command == 1) copy();
        return true;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        const bool horizontal = message == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(a) & MK_SHIFT);
        wheel_ += GET_WHEEL_DELTA_WPARAM(a) * (message == WM_MOUSEHWHEEL ? -1 : 1);
        const int steps = wheel_ / WHEEL_DELTA; wheel_ %= WHEEL_DELTA;
        if (horizontal) left_ -= steps * columnWidth() / 2; else top_ -= steps * 3;
        layout(); InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_); return true;
    }
    case WM_VSCROLL: case WM_HSCROLL: {
        const bool vertical = message == WM_VSCROLL; int& at = vertical ? top_ : left_;
        SCROLLINFO info{sizeof(info), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window_, vertical ? SB_VERT : SB_HORZ, &info);
        switch (LOWORD(a)) {
        case SB_LINEUP: at -= vertical ? 1 : px(24); break;
        case SB_LINEDOWN: at += vertical ? 1 : px(24); break;
        case SB_PAGEUP: at -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: at += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: at = scrollbars_->trackPosition(vertical ? SB_VERT : SB_HORZ, info.nTrackPos); break;
        case SB_TOP: at = 0; break;
        case SB_BOTTOM: at = info.nMax; break;
        default: return true;
        }
        layout(); InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_); return true;
    }
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && a == 'C') { copy(); return true; }
        if (ctrl && a == 'A') { if (rows()) { anchorRow_ = anchorColumn_ = 0; row_ = rows() - 1; column_ = columns() - 1; InvalidateRect(window_, nullptr, FALSE); if (statusChanged) statusChanged(); } return true; }
        if (a == VK_ESCAPE && selecting_) { stopDrag(); return true; }
        if (a == VK_F8) { nextChange(shift); return true; }
        if (a == VK_UP || a == VK_DOWN || a == VK_PRIOR || a == VK_NEXT || a == VK_LEFT || a == VK_RIGHT || a == VK_HOME || a == VK_END || a == VK_TAB) { move(a, shift, ctrl); return true; }
        return false;
    }
    case WM_CHAR: return true;
    }
    return false;
}
}
