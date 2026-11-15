#include "csv_view.h"
#include "selection_scroll.h"
#include <algorithm>

namespace poppy {
namespace {
constexpr COLORREF selectedCell = RGB(43, 62, 55), activeHeader = RGB(43, 67, 52), matchedCell = RGB(92, 73, 34);
std::wstring displayValue(std::wstring text) {
    // Keep UTF-16 offsets intact for the formula input's selection and IME.
    for (auto& ch : text) {
        if (ch == L'\r') ch = L'␍';
        else if (ch == L'\n') ch = L'↵';
        else if (ch == L'\t') ch = L'⇥';
    }
    return text;
}
}
void CsvView::attach(HWND window, Renderer& renderer, Scrollbars& scrollbars) {
    window_ = window; renderer_ = &renderer; scrollbars_ = &scrollbars;
}
void CsvView::setDpi(UINT dpi) { dpi_ = dpi; if (active_) layout(); }
void CsvView::show(std::shared_ptr<const Document> document, bool preserve) {
    finishEdit(); if (!dragHeld_) stopDrag();
    document_ = std::move(document); edit_.reset(); active_ = true; dragLoading_ = false;
    settle();
    if (!preserve) { top_ = left_ = 0; anchor_ = caret_ = {}; widths_.clear(); }
    rebuild(); layout();
}
void CsvView::hide(bool release) {
    finishEdit(); if (release || !dragHeld_) stopDrag(); active_ = false;
    search_ = {};
    if (release) { document_.reset(); edit_.reset(); table_ = {}; widths_ = {}; edges_ = {}; text_ = {}; }
}
void CsvView::holdDrag(bool hold) {
    dragHeld_ = hold;
    // The drag outlives its hold only in a table showing the new window that
    // still has the mouse captured.
    if (!hold && selecting_ && (!active_ || !valid_ || GetCapture() != window_)) stopDrag();
}
void CsvView::rebuild() {
    valid_ = document_ && (partial() ? table_.parseWindow(document_->text, document_->tailOffset <= 3) : table_.parse(document_->text));
    if (valid_) {
        caret_.row = std::min(caret_.row, gridRows() - 1); anchor_.row = std::min(anchor_.row, gridRows() - 1);
        caret_.column = std::min(caret_.column, gridColumns() - 1); anchor_.column = std::min(anchor_.column, gridColumns() - 1);
    }
}
void CsvView::refresh() {
    finishEdit(); rebuild(); layout(); InvalidateRect(window_, nullptr, FALSE);
}
int CsvView::rowGutter() const {
    // Size a large file's gutter for its byte count, which bounds both line
    // numbers and byte offsets, so loading another window never shifts cells.
    const auto digits = partial() ? std::to_string(document_->fileBytes + 1).size() : std::to_wstring(gridRows()).size();
    return std::max(px(52), static_cast<int>(digits) * renderer_->cellWidth() + px(18));
}
int CsvView::nameWidth() const {
    if (!partial()) return px(78);
    return std::max(px(78), static_cast<int>(std::to_string(document_->fileBytes + 1).size() + 3) * renderer_->cellWidth() + px(12));
}
std::wstring CsvView::rowName(size_t row) const {
    if (!partial() || !rowLabel) return std::to_wstring(row + 1);
    return valid_ && row < table_.rows() ? rowLabel(rowOffset(row)) : std::wstring{};
}
void CsvView::scrollTo(int top, std::optional<int> left) {
    top_ = top; if (left) left_ = *left;
    layout();
    // A drag selects through the cell that scrolled under the pointer.
    if (selecting_ && valid_) { dragCaret(); dragStatus(); }
    InvalidateRect(window_, nullptr, FALSE);
}
void CsvView::selectMarks(Mark anchor, Mark caret, std::wstring caretName, std::wstring caretValue) {
    if (!valid_) return;
    // A cell outside the window stands at the edge it lies beyond, so the
    // selected range still reaches that edge, and keeps its own column.
    const auto place = [&](const Mark& mark) {
        if (!mark.side) return table_.position(mark.offset);
        return CsvPosition{mark.side < 0 ? 0 : gridRows() - 1, std::min(mark.column, gridColumns() - 1)};
    };
    anchor_ = place(anchor); caret_ = place(caret); anchorSide_ = anchor.side; caretSide_ = caret.side;
    caretName_ = std::move(caretName); caretValue_ = std::move(caretValue);
    InvalidateRect(window_, nullptr, FALSE);
}
std::wstring CsvView::caretValue() const {
    if (caretSide_) return caretValue_;
    return valid_ ? wide(table_.value(document_->text, caret_)) : std::wstring{};
}
void CsvView::moveCaret(size_t row, size_t column, bool extend) {
    if (!valid_) return;
    select({std::min(row, gridRows() - 1), std::min(column, gridColumns() - 1)}, extend);
}
int CsvView::pageRows() const { return std::max(1, static_cast<int>(Scrollbars::contentRect(window_).bottom - bodyTop()) / rowHeight()); }
RECT CsvView::formula() const {
    const auto rect = Scrollbars::contentRect(window_);
    const int left = nameWidth() + px(4);
    return {left, insetTop() + px(4), std::max<LONG>(left, rect.right - px(6)), insetTop() + px(30)};
}
RECT CsvView::cellRect(CsvPosition cell) const {
    if (cell.column + 1 >= edges_.size()) return {};
    const auto x = rowGutter() - left_;
    const int y = bodyTop() + (static_cast<int>(cell.row) - top_) * rowHeight();
    return {x + edges_[cell.column], y, x + edges_[cell.column + 1], y + rowHeight()};
}
RECT CsvView::textRect(CsvPosition cell) const {
    auto rect = cellRect(cell); rect.left += px(7); rect.right -= px(6); return rect;
}
void CsvView::layout() {
    if (!active_ || !window_) return;
    widths_.resize(gridColumns(), 160); edges_.resize(widths_.size() + 1); edges_[0] = 0;
    for (size_t c = 0; c < widths_.size(); ++c) edges_[c + 1] = edges_[c] + px(widths_[c]);
    for (int pass = 0; pass < 3; ++pass) {
        const auto r = Scrollbars::contentRect(window_);
        const int rows = valid_ ? static_cast<int>(gridRows()) : 0;
        const int columns = valid_ ? edges_.back() : 0;
        const int page = pageRows(), width = std::max(1, static_cast<int>(r.right) - rowGutter());
        top_ = std::clamp(top_, 0, std::max(0, rows - page)); left_ = std::clamp(left_, 0, std::max(0, columns - width));
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0, rows - 1, static_cast<UINT>(page), top_, 0};
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0, columns - 1, static_cast<UINT>(width), left_, 0};
        if (partial() && fileScroll) fileScroll(vertical, rows, page);
        SetScrollInfo(window_, SB_VERT, &vertical, FALSE); SetScrollInfo(window_, SB_HORZ, &horizontal, FALSE);
    }
    inputChanged();
}
void CsvView::reveal() {
    const auto r = Scrollbars::contentRect(window_);
    const auto cell = cellRect(caret_);
    if (caret_.row < static_cast<size_t>(top_)) top_ = static_cast<int>(caret_.row);
    else if (cell.bottom > r.bottom) top_ += (cell.bottom - r.bottom + rowHeight() - 1) / rowHeight();
    if (cell.left < rowGutter() || cell.right - cell.left > r.right - rowGutter()) left_ = edges_[caret_.column];
    else if (cell.right > r.right) left_ += cell.right - r.right;
    layout();
}
CsvPosition CsvView::hit(POINT point) const {
    if (!valid_ || edges_.empty()) return {};
    const int x = std::max(0, static_cast<int>(point.x) - rowGutter() + left_);
    const auto column = std::upper_bound(edges_.begin(), edges_.end(), x);
    return {std::min(gridRows() - 1, static_cast<size_t>(std::max(0, static_cast<int>(point.y) - bodyTop()) / rowHeight() + top_)),
        std::min(gridColumns() - 1, static_cast<size_t>(column - edges_.begin() - 1))};
}
CsvRange CsvView::selection() const {
    return {std::min(anchor_.row, caret_.row), std::min(anchor_.column, caret_.column), std::max(anchor_.row, caret_.row), std::max(anchor_.column, caret_.column)};
}
int CsvView::resizeColumn(POINT point) const {
    if (!valid_ || point.y < headerTop() || point.y >= bodyTop() || point.x < rowGutter()) return -1;
    const int x = point.x - rowGutter() + left_;
    const auto edge = std::lower_bound(edges_.begin() + 1, edges_.end(), x - px(4));
    return edge != edges_.end() && std::abs(*edge - x) <= px(4) ? static_cast<int>(edge - edges_.begin() - 1) : -1;
}
void CsvView::select(CsvPosition cell, bool extend, bool scrollIntoView) {
    finishEdit(); caret_ = cell; caretSide_ = 0; if (!extend) { anchor_ = caret_; anchorSide_ = 0; }
    if (scrollIntoView) reveal(); else inputChanged();
    InvalidateRect(window_, nullptr, FALSE); if (statusChanged) statusChanged();
}
void CsvView::drag(POINT point) {
    dragPoint_ = point; dragCaret();
    InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_); dragStatus();
}
void CsvView::dragStatus(bool flush) {
    const auto now = std::chrono::steady_clock::now();
    if (!flush && now - statusTick_ < std::chrono::milliseconds(100)) {
        if (!statusPending_) SetTimer(window_, statusTimer, 100, nullptr);
        statusPending_ = true; return;
    }
    if (statusPending_) KillTimer(window_, statusTimer);
    statusPending_ = false; statusTick_ = now;
    if (statusChanged) statusChanged();
}
void CsvView::dragCaret() {
    auto point = dragPoint_;
    const auto r = Scrollbars::contentRect(window_);
    point.x = std::clamp<LONG>(point.x, rowGutter(), std::max<LONG>(rowGutter(), r.right - 1));
    point.y = std::clamp<LONG>(point.y, bodyTop(), std::max<LONG>(bodyTop(), r.bottom - 1));
    caret_ = hit(point); caretSide_ = 0;
    if (dragHeader_ == 1) caret_.column = table_.columns() - 1;
    if (dragHeader_ == 2) caret_.row = table_.rows() - 1;
}
void CsvView::stopDrag() {
    if (!window_) return;
    dragScrolling_ = false;
    if (statusPending_) dragStatus(true);
    const bool captured = selecting_ || inputSelecting_ || resizing_ >= 0;
    const bool selecting = selecting_;
    selecting_ = inputSelecting_ = false; resizing_ = -1;
    if (captured && GetCapture() == window_) ReleaseCapture();
    if (selecting) inputChanged();
}
// Called at the start of every frame while a selection is being dragged. It
// scrolls by the time since the previous frame, so the speed follows the
// pointer exactly and the next frame is requested as soon as this one is
// presented, without waiting on low-priority WM_TIMER messages.
bool CsvView::advanceDrag() {
    const auto r = Scrollbars::contentRect(window_); const int rh = rowHeight();
    // Rows per second: rows scroll whole, like the grid's other scrolling.
    const double vertical = dragHeader_ != 2 ? selectionScrollSpeed(dragPoint_.y, bodyTop(), r.bottom, rh) : 0;
    const double horizontal = dragHeader_ != 1 ? selectionScrollSpeed(dragPoint_.x, rowGutter(), r.right, rh) * px(24) : 0;
    const auto now = std::chrono::steady_clock::now();
    // The first frame outside moves at once instead of idling: a whole row
    // vertically, a frame's worth horizontally.
    const double seconds = dragScrolling_ ? std::min(0.05, std::chrono::duration<double>(now - dragTick_).count()) : 1.0 / 60;
    if (!dragScrolling_) dragCarryY_ = vertical < 0 ? -1 : vertical > 0 ? 1 : 0;
    dragTick_ = now;
    const auto advance = [seconds](double& carry, double speed) {
        if (!speed) { carry = 0; return 0; }
        carry += speed * seconds; const int units = static_cast<int>(carry); carry -= units; return units;
    };
    const int top = top_, left = left_;
    top_ += advance(dragCarryY_, vertical); left_ += advance(dragCarryX_, horizontal);
    layout();
    // Keep requesting frames while a partial row accumulates, and stop once
    // the table is at its limit in every direction the pointer points.
    const int bottom = std::max(0, static_cast<int>(gridRows()) - pageRows());
    const int right = std::max(0, edges_.back() - std::max(1, static_cast<int>(r.right) - rowGutter()));
    dragScrolling_ = (vertical < 0 && top_ > 0) || (vertical > 0 && top_ < bottom) || (horizontal < 0 && left_ > 0) || (horizontal > 0 && left_ < right);
    if (top != top_ || left != left_) { dragCaret(); dragStatus(); }
    // Once the drag reaches either end of a large file's window, which stops
    // the frames above, the owner loads the records beyond it. The window it
    // shows repaints, so scrolling goes on without the pointer moving.
    const int edge = vertical < 0 && top_ <= 0 ? -1 : vertical > 0 && top_ >= bottom ? 1 : 0;
    if (edge && partial() && loadRows && !dragLoading_) { dragLoading_ = true; loadRows(edge); }
    return dragScrolling_;
}
void CsvView::move(WPARAM key, bool shift, bool ctrl) {
    auto cell = caret_;
    const bool vertical = key == VK_UP || key == VK_DOWN || key == VK_RETURN || key == VK_PRIOR || key == VK_NEXT;
    if (partial() && moveRows && (vertical || (ctrl && (key == VK_HOME || key == VK_END)))) {
        const bool backwards = key == VK_UP || key == VK_PRIOR || key == VK_HOME || (key == VK_RETURN && shift);
        const bool extend = shift && key != VK_RETURN;
        if (ctrl && key != VK_RETURN) {
            if (moveRows(backwards ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max(), extend)) return;
        } else {
            const int64_t step = key == VK_PRIOR || key == VK_NEXT ? pageRows() : 1;
            const int64_t target = static_cast<int64_t>(cell.row) + (backwards ? -step : step);
            // A caret outside the window moves from its own record.
            if ((caretSide_ || target < 0 || target >= static_cast<int64_t>(table_.rows())) && moveRows(backwards ? -step : step, extend)) return;
        }
    } else if (partial() && moveRows && caretSide_ && (key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END || key == VK_TAB)) {
        // A sideways move of a caret outside the window loads its record.
        int64_t rows = 0; size_t column = caret_.column;
        const size_t columns = std::max<size_t>(1, table_.columns());
        if (key == VK_HOME) column = 0;
        else if (key == VK_END) column = columns - 1;
        else if (key == VK_TAB) {
            if (shift) { if (column) --column; else { rows = -1; column = columns - 1; } }
            else if (column + 1 < columns) ++column;
            else { rows = 1; column = 0; }
        } else if (key == VK_LEFT) column = ctrl ? 0 : column - std::min<size_t>(column, 1);
        else column = ctrl ? columns - 1 : std::min(gridColumns() - 1, column + 1);
        const auto previous = caret_.column; caret_.column = column;
        if (moveRows(rows, shift && key != VK_TAB)) return;
        caret_.column = previous;
    }
    if (key == VK_HOME) { cell.column = 0; if (ctrl) cell.row = 0; }
    else if (key == VK_END) { cell.column = table_.columns() - 1; if (ctrl) cell.row = table_.rows() - 1; }
    else if (key == VK_TAB) {
        if (shift) { if (cell.column) --cell.column; else if (cell.row) { --cell.row; cell.column = table_.columns() - 1; } }
        else if (cell.column + 1 < table_.columns()) ++cell.column;
        else { cell.column = 0; cell.row = std::min(gridRows() - 1, cell.row + 1); }
    } else {
        const int direction = key == VK_UP || key == VK_LEFT || key == VK_PRIOR || (key == VK_RETURN && shift) ? -1 : 1;
        auto& at = vertical ? cell.row : cell.column;
        const size_t limit = (vertical ? gridRows() : gridColumns()) - 1;
        const size_t step = key == VK_PRIOR || key == VK_NEXT ? static_cast<size_t>(pageRows()) : 1;
        if (ctrl) at = direction < 0 ? 0 : (vertical ? table_.rows() : table_.columns()) - 1;
        else at = direction < 0 ? at - std::min(at, step) : std::min(limit, at + step);
    }
    select(cell, shift && key != VK_TAB && key != VK_RETURN);
}
bool CsvView::apply(const std::optional<CsvReplacement>& replacement, bool join) {
    if (!edit_ || !replacement || !edit_->replace(replacement->begin, replacement->end, replacement->text, join)) {
        MessageBeep(MB_ICONWARNING); return false;
    }
    rebuild(); layout(); InvalidateRect(window_, nullptr, FALSE);
    if (changed) changed();
    if (statusChanged) statusChanged();
    return true;
}
void CsvView::copy(bool cut) {
    // Cells outside a large file's window are not loaded and cannot be copied.
    if (!valid_ || anchorSide_ || caretSide_) { MessageBeep(MB_ICONWARNING); return; }
    const auto value = table_.copy(document_->text, selection());
    if (!value || !copyText(window_, wide(*value))) { MessageBeep(MB_ICONWARNING); return; }
    if (cut && edit_) clear();
}
bool CsvView::appendable() const {
    return !partial() || (document_->tailEndOffset >= document_->fileBytes && !table_.truncated());
}
void CsvView::paste() {
    if (!edit_ || !valid_) return;
    if (!editable()) { MessageBeep(MB_ICONWARNING); return; }
    const auto text = utf8(clipboardText(window_)); if (text.empty()) return;
    CsvTable clipboard;
    CsvValues values;
    if (clipboard.parse(text, '\t')) values = clipboard.values(text);
    else values = {{text}};
    const auto range = selection();
    if (values.size() == 1 && values[0].size() == 1) {
        const size_t rows = range.lastRow - range.firstRow + 1, columns = range.lastColumn - range.firstColumn + 1;
        if (rows > CsvTable::maxCells / columns || (!values[0][0].empty() && rows * columns > maxFileBytes / values[0][0].size())) { MessageBeep(MB_ICONWARNING); return; }
        values = CsvValues(rows, std::vector<std::string>(columns, values[0][0]));
    }
    // Rows beyond a large file's window would be inserted before the rows
    // that follow it in the file rather than replace them.
    if (range.firstRow + values.size() > table_.rows() && !appendable()) { MessageBeep(MB_ICONWARNING); return; }
    if (apply(table_.replace(document_->text, {range.firstRow, range.firstColumn}, values))) {
        size_t columns = 0;
        for (const auto& row : values) columns = std::max(columns, row.size());
        anchor_ = {range.firstRow, range.firstColumn}; caret_ = {range.firstRow + values.size() - 1, range.firstColumn + columns - 1};
        reveal(); InvalidateRect(window_, nullptr, FALSE);
    }
}
void CsvView::clear() {
    if (!edit_ || !valid_) return;
    if (!editable()) { MessageBeep(MB_ICONWARNING); return; }
    apply(table_.clear(document_->text, selection()));
}
void CsvView::undo(bool redo) {
    if (!edit_) return;
    finishEdit(); size_t anchor = 0, caret = 0;
    if (redo ? edit_->redo(anchor, caret) : edit_->undo(anchor, caret)) {
        rebuild(); anchor_ = caret_ = table_.position(caret); settle(); reveal(); InvalidateRect(window_, nullptr, FALSE);
        if (changed) changed();
        if (statusChanged) statusChanged();
    }
}
void CsvView::startEdit(bool empty, bool formula) {
    if (!edit_ || !valid_) return;
    if (!editable()) { MessageBeep(MB_ICONWARNING); return; }
    stopDrag(); editing_ = true; formulaEdit_ = formula; editChanged_ = false; editRevision_ = edit_->revision;
    text_ = empty ? std::wstring{} : wide(table_.value(document_->text, caret_));
    textCaret_ = textAnchor_ = text_.size(); highSurrogate_ = 0; input_.reset();
    // An in-cell edit scrolls its whole cell into view; reveal() lays it out.
    if (formula) inputChanged(); else reveal();
    InvalidateRect(window_, nullptr, FALSE);
}
void CsvView::finishEdit(bool cancel) {
    if (!editing_) return;
    editing_ = formulaEdit_ = false; editBox_ = {}; ime_.cancel(window_); highSurrogate_ = 0;
    if (cancel && editChanged_ && edit_ && edit_->revision == editRevision_) {
        size_t anchor = 0, caret = 0;
        if (edit_->undo(anchor, caret)) {
            rebuild(); layout();
            if (changed) changed();
            if (statusChanged) statusChanged();
        }
    }
    inputChanged(); InvalidateRect(window_, nullptr, FALSE);
}
void CsvView::inputChanged() {
    if (!renderer_ || !window_) return;
    const bool focused = GetFocus() == window_;
    // Cells use the UI font, so the IME's own windows and metadata match the
    // proportional text drawn in the cell and the formula bar.
    const auto& font = renderer_->compositionFont(Renderer::Font::Ui);
    if (!editing_) {
        editBox_ = {};
        // Typing or composing starts an in-cell edit at the selected cell's
        // text origin, so keep the IME there until then.
        // A drag repositions it once it stops rather than on every frame.
        if (focused && active_ && valid_ && !selecting_ && !caretSide_ && caret_.column + 1 < edges_.size()) {
            input_.update(*renderer_, {}, 0, 0, ime_.text(), textRect(caret_));
            positionIme(window_, input_.caret(), input_.height(), font);
        }
        return;
    }
    const auto display = displayValue(text_);
    if (formulaEdit_) {
        auto rect = formula(); rect.left += px(6); rect.right -= px(4);
        input_.update(*renderer_, display, textCaret_, textAnchor_, ime_.text(), rect); editBox_ = {};
    } else {
        // Lay the edit out in exactly the rect the cell's label uses.
        auto box = cellRect(caret_), rect = textRect(caret_); const auto cellRight = box.right;
        // Like a spreadsheet, text wider than its cell covers the following
        // columns, and scrolls only once it reaches the table's right edge.
        // Measure in the widest box so a narrower pass never scrolls it.
        const auto widest = std::max(cellRight, Scrollbars::contentRect(window_).right);
        const RECT measure{rect.left, rect.top, std::max(rect.right, widest - px(6)), rect.bottom};
        input_.update(*renderer_, display, textCaret_, textAnchor_, ime_.text(), measure);
        const LONG needed = rect.left + input_.width() + input_.caretWidth() + px(6);
        if (needed > box.right) {
            const int origin = rowGutter() - left_;
            for (size_t edge = caret_.column + 1; box.right < needed && edge + 1 < edges_.size();) box.right = origin + edges_[++edge];
            box.right = std::max(cellRight, std::min(std::max(box.right, needed), widest));
        }
        rect.right = box.right - px(6);
        if (rect.right != measure.right) input_.update(*renderer_, display, textCaret_, textAnchor_, ime_.text(), rect);
        editBox_ = box;
    }
    if (focused) positionIme(window_, input_.caret(), input_.height(), font);
    InvalidateRect(window_, nullptr, FALSE);
}
size_t CsvView::previous(size_t offset) const {
    if (!offset) return 0;
    --offset;
    if (offset && text_[offset] >= 0xdc00 && text_[offset] <= 0xdfff && text_[offset - 1] >= 0xd800 && text_[offset - 1] <= 0xdbff) --offset;
    return offset;
}
size_t CsvView::next(size_t offset) const {
    if (offset == text_.size()) return offset;
    const auto ch = text_[offset++];
    if (ch >= 0xd800 && ch <= 0xdbff && offset < text_.size() && text_[offset] >= 0xdc00 && text_[offset] <= 0xdfff) ++offset;
    return offset;
}
void CsvView::replaceInput(std::wstring text) {
    if (!editing_ || !edit_) return;
    const size_t begin = std::min(textAnchor_, textCaret_), end = std::max(textAnchor_, textCaret_);
    auto proposed = text_; proposed.replace(begin, end - begin, text);
    const auto revision = edit_->revision;
    if (apply(table_.replace(document_->text, caret_, {{utf8(proposed)}}), editChanged_ && revision == editRevision_)) {
        editChanged_ = editChanged_ || edit_->revision != revision; editRevision_ = edit_->revision;
        text_ = std::move(proposed); textCaret_ = textAnchor_ = begin + text.size();
        // Typing brings a cell scrolled out of view back, with the IME on it.
        if (formulaEdit_) inputChanged(); else reveal();
    }
}
bool CsvView::inputKey(WPARAM key, bool ctrl, bool shift) {
    if (ctrl && key == 'A') { textAnchor_ = 0; textCaret_ = text_.size(); inputChanged(); return true; }
    if (ctrl && (key == 'C' || key == 'X')) {
        if (textAnchor_ != textCaret_ && copyText(window_, text_.substr(std::min(textAnchor_, textCaret_), std::max(textAnchor_, textCaret_) - std::min(textAnchor_, textCaret_))) && key == 'X') replaceInput(L"");
        return true;
    }
    if ((ctrl && key == 'V') || (shift && key == VK_INSERT)) { replaceInput(clipboardText(window_)); return true; }
    if (key == VK_ESCAPE) { finishEdit(true); return true; }
    if (key == VK_TAB || key == VK_RETURN) { finishEdit(); move(key, shift, false); return true; }
    if (key == VK_BACK) { if (textAnchor_ == textCaret_) textAnchor_ = ctrl ? 0 : previous(textCaret_); replaceInput(L""); return true; }
    if (key == VK_DELETE) { if (textAnchor_ == textCaret_) textAnchor_ = ctrl ? text_.size() : next(textCaret_); replaceInput(L""); return true; }
    if (key == VK_LEFT) textCaret_ = !shift && textAnchor_ != textCaret_ ? std::min(textAnchor_, textCaret_) : ctrl ? 0 : previous(textCaret_);
    else if (key == VK_RIGHT) textCaret_ = !shift && textAnchor_ != textCaret_ ? std::max(textAnchor_, textCaret_) : ctrl ? text_.size() : next(textCaret_);
    else if (key == VK_HOME) textCaret_ = 0;
    else if (key == VK_END) textCaret_ = text_.size();
    else return false;
    if (!shift) textAnchor_ = textCaret_;
    inputChanged(); return true;
}
ViewState CsvView::state() const {
    if (!document_ || !valid_) return {};
    const auto offset = table_.offset({static_cast<size_t>(top_), 0});
    const auto line = std::upper_bound(document_->lines.begin(), document_->lines.end(), offset);
    return {static_cast<int>(line - document_->lines.begin()) - 1, left_, table_.offset(anchor_), table_.offset(caret_)};
}
void CsvView::restore(const ViewState& state) {
    if (!document_ || !valid_) return;
    finishEdit();
    const auto row = std::min(static_cast<size_t>(std::max(0, state.top)), document_->lines.size() - 1);
    top_ = static_cast<int>(table_.position(document_->lines[row]).row); left_ = state.left;
    anchor_ = table_.position(state.anchor); caret_ = table_.position(state.caret); settle(); layout(); InvalidateRect(window_, nullptr, FALSE);
}
std::wstring CsvView::status() const {
    if (!valid_) return L"Invalid CSV · switch to Source";
    const auto range = selection();
    return wide(csvColumnName(caret_.column)) + caretName() + L"  ·  " +
        std::to_wstring(table_.rows()) + (partial() ? L" rows loaded × " : L" rows × ") + std::to_wstring(table_.columns()) + L" columns" +
        (!anchorSide_ && !caretSide_ && (range.firstRow != range.lastRow || range.firstColumn != range.lastColumn) ? L"  ·  " + std::to_wstring(range.lastRow - range.firstRow + 1) + L" × " + std::to_wstring(range.lastColumn - range.firstColumn + 1) + L" selected" : L"");
}
std::optional<std::wstring> CsvView::searchSeed() const {
    if (!valid_) return {};
    auto value = table_.value(document_->text, caret_);
    if (value.empty() || value.size() > 2048 || value.find_first_of("\r\n") != std::string::npos) return {};
    return wide(value);
}
void CsvView::find(std::string query, bool matchCase, bool navigate, bool backwards) {
    search_.query(std::move(query), matchCase);
    // Typing keeps a still-matching current cell; Enter / F3 move past it.
    search_.start(valid_ ? caret_ : CsvPosition{}, backwards, navigate && !backwards);
    InvalidateRect(window_, nullptr, FALSE);
}
bool CsvView::findStep(size_t cellBudget) {
    return !valid_ || !document_ || search_.step(table_, document_->text, cellBudget);
}
void CsvView::revealFind() {
    if (valid_ && search_.current()) select(*search_.current());
    else InvalidateRect(window_, nullptr, FALSE);
}
void CsvView::clearFind() { search_ = {}; if (window_) InvalidateRect(window_, nullptr, FALSE); }
void CsvView::paint() {
    const bool scrolling = selecting_ && advanceDrag();
    Renderer::Paint paint(*renderer_, window_);
    // Invalidate after BeginPaint so the request survives into the next frame.
    if (scrolling) InvalidateRect(window_, nullptr, FALSE);
    if (paint) draw();
}
void CsvView::draw() {
    auto& dc = *renderer_; dc.font(Renderer::Font::Ui);
    const auto rect = Scrollbars::contentRect(window_);
    dc.fill(rect, background);
    if (!valid_) {
        dc.label({px(24), px(24), rect.right - px(24), rect.bottom}, L"CSV table unavailable\n\n" + wide(table_.error()) + L"\n\nSwitch to Source to view or fix this file.", muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        scrollbars_->paint(window_, dc, background); return;
    }
    const auto range = selection(); const bool loaded = selectionLoaded(); const int rh = rowHeight(), gutter = rowGutter();
    const bool inCell = editingInCell();
    const size_t firstColumn = hit({gutter, bodyTop()}).column;
    const size_t lastColumn = hit({rect.right, bodyTop()}).column;
    const size_t lastRow = std::min(gridRows(), static_cast<size_t>(top_ + std::max(0, static_cast<int>(rect.bottom) - bodyTop() + rh - 1) / rh));
    for (size_t r = static_cast<size_t>(top_); r < lastRow; ++r) {
        for (size_t c = firstColumn; c <= lastColumn; ++c) {
            auto cell = cellRect({r, c}); const int right = cell.right;
            cell.left = std::max<LONG>(gutter, cell.left); cell.right = std::min(cell.right, rect.right); cell.bottom = std::min(cell.bottom, rect.bottom);
            if (cell.right <= cell.left) continue;
            const bool selected = loaded && r >= range.firstRow && r <= range.lastRow && c >= range.firstColumn && c <= range.lastColumn;
            dc.fill(cell, selected ? selectedCell : !search_.empty() && search_.matches(table_, document_->text, {r, c}) ? matchedCell : background);
            dc.fill({cell.left, cell.bottom - 1, cell.right, cell.bottom}, border);
            if (right <= rect.right) dc.fill({right - 1, cell.top, right, cell.bottom}, border);
            // The in-cell input draws the edited value itself, below.
            if (!inCell || r != caret_.row || c != caret_.column)
                dc.label(textRect({r, c}), cell, displayValue(wide(table_.value(document_->text, {r, c}))), foreground);
        }
        const int y = bodyTop() + (static_cast<int>(r) - top_) * rh;
        const RECT rowHeader{0, y, std::min<LONG>(gutter, rect.right), std::min<LONG>(y + rh, rect.bottom)};
        dc.fill(rowHeader, loaded && r >= range.firstRow && r <= range.lastRow ? activeHeader : panel);
        dc.label({0, y, gutter, y + rh}, rowHeader, rowName(r), !caretSide_ && r == caret_.row ? accent : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        dc.fill({0, rowHeader.bottom - 1, rowHeader.right, rowHeader.bottom}, border);
    }
    // Fill the caret frame as whole-pixel strips over the cell's grid lines so it never straddles a pixel boundary.
    // A caret outside a large file's window has no frame here.
    const RECT clip{gutter, bodyTop(), rect.right, rect.bottom}; const int thick = std::max(1, px(2));
    if (inCell) {
        // The cell itself is drawn as usual, so an edit that fits differs from
        // it only by the caret. A wider box covers the cell's right grid line
        // and the following cells with the selected background; the frame
        // below covers the grid lines along its edges.
        const auto cellRight = cellRect(caret_).right; RECT visible{};
        const RECT extension{cellRight - 1, editBox_.top, editBox_.right, editBox_.bottom};
        if (editBox_.right > cellRight && IntersectRect(&visible, &extension, &clip)) dc.fill(visible, selectedCell);
        input_.draw(dc, foreground, RGB(50, 70, 94), GetFocus() == window_, &clip);
    }
    auto current = inCell ? editBox_ : cellRect(caret_); current.left -= 1; current.top -= 1;
    if (!caretSide_) for (const RECT& strip : {RECT{current.left, current.top, current.right, current.top + thick}, RECT{current.left, current.bottom - thick, current.right, current.bottom},
                              RECT{current.left, current.top, current.left + thick, current.bottom}, RECT{current.right - thick, current.top, current.right, current.bottom}}) {
        RECT visible{}; if (IntersectRect(&visible, &strip, &clip)) dc.fill(visible, accent);
    }
    dc.fill({0, headerTop(), rect.right, std::min<LONG>(bodyTop(), rect.bottom)}, panel);
    for (size_t c = firstColumn; c <= lastColumn; ++c) {
        const auto cell = cellRect({static_cast<size_t>(top_), c});
        const RECT header{std::max<LONG>(gutter, cell.left), headerTop(), std::min(cell.right, rect.right), std::min<LONG>(bodyTop(), rect.bottom)};
        dc.fill(header, c >= range.firstColumn && c <= range.lastColumn ? activeHeader : panel);
        dc.label({cell.left, headerTop(), cell.right, bodyTop()}, header, wide(csvColumnName(c)), c == caret_.column ? accent : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        dc.fill({header.right - 1, header.top, header.right, header.bottom}, border);
    }
    dc.fill({0, headerTop(), std::min<LONG>(gutter, rect.right), std::min<LONG>(bodyTop(), rect.bottom)}, panel);
    dc.fill({0, bodyTop() - 1, rect.right, bodyTop()}, border);
    dc.fill({gutter - 1, headerTop(), gutter, rect.bottom}, border);
    dc.fill({0, insetTop(), rect.right, std::min<LONG>(headerTop(), rect.bottom)}, panel);
    dc.label({0, insetTop(), nameWidth(), headerTop()}, wide(csvColumnName(caret_.column)) + caretName(), accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const auto box = formula(); dc.fill(box, background); dc.outline(box, editing_ && formulaEdit_ ? accent : border);
    if (editing_ && formulaEdit_) input_.draw(dc, foreground, RGB(50, 70, 94), GetFocus() == window_);
    else dc.label({box.left + px(6), box.top, box.right - px(4), box.bottom}, displayValue(caretValue()), foreground);
    scrollbars_->paint(window_, dc, background);
}
bool CsvView::handle(UINT message, WPARAM a, LPARAM b, LRESULT& result) {
    result = 0;
    switch (message) {
    case WM_PAINT: paint(); return true;
    case WM_SIZE: layout(); InvalidateRect(window_, nullptr, FALSE); return true;
    case WM_GETDLGCODE: result = DLGC_WANTALLKEYS; return true;
    case WM_SETFOCUS: inputChanged(); InvalidateRect(window_, nullptr, FALSE); return false;
    case WM_KILLFOCUS: finishEdit(); stopDrag(); ime_.cancel(window_); return false;
    case WM_SHOWWINDOW: if (!a) { finishEdit(); stopDrag(); } return false;
    case WM_DESTROY: hide(true); return false;
    case WM_SETCURSOR:
        if (LOWORD(b) == HTCLIENT) {
            POINT point{}; GetCursorPos(&point); ScreenToClient(window_, &point); const auto box = formula();
            const bool text = (PtInRect(&box, point) && edit_) || (editingInCell() && PtInRect(&editBox_, point) && point.x >= rowGutter() && point.y >= bodyTop());
            SetCursor(LoadCursorW(nullptr, resizeColumn(point) >= 0 ? IDC_SIZEWE : text ? IDC_IBEAM : IDC_ARROW)); result = TRUE; return true;
        }
        return false;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        SetFocus(window_); if (!valid_) return true;
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)}; const auto box = formula();
        // Like the Explorer's rename input, ignore clicks while composing:
        // hit() indexes the displayed text, which includes the preedit.
        if (ime_.active()) return true;
        const bool formulaHit = PtInRect(&box, point) && edit_;
        if (formulaHit && !editing_) startEdit(false, true);
        // The formula bar takes over an in-cell edit with its text and caret.
        else if (formulaHit && !formulaEdit_) { formulaEdit_ = true; input_.reset(); inputChanged(); }
        if (editing_ && (formulaHit || (!formulaEdit_ && PtInRect(&editBox_, point) && point.x >= rowGutter() && point.y >= bodyTop()))) {
            textCaret_ = std::min(input_.hit(point.x), text_.size()); if (!(a & MK_SHIFT)) textAnchor_ = textCaret_;
            inputSelecting_ = true; SetCapture(window_); inputChanged(); return true;
        }
        if (formulaHit) return true;
        finishEdit();
        const auto bounds = Scrollbars::contentRect(window_); if (!PtInRect(&bounds, point) || point.y < headerTop()) return true;
        resizing_ = resizeColumn(point);
        if (resizing_ >= 0) { resizeX_ = point.x; resizeWidth_ = widths_[resizing_]; SetCapture(window_); return true; }
        if (point.x < rowGutter() && point.y < bodyTop()) {
            anchor_ = {}; caret_ = {table_.rows() - 1, table_.columns() - 1}; settle(); InvalidateRect(window_, nullptr, FALSE); if (statusChanged) statusChanged(); return true;
        }
        // Scrolling a partial cell under a stationary pointer would make the
        // following mouse-up select a different row or column.
        select(hit(point), (a & MK_SHIFT) != 0, false);
        dragHeader_ = point.x < rowGutter() ? 1 : point.y < bodyTop() ? 2 : 0;
        if (dragHeader_ == 1) { anchor_.column = 0; caret_.column = table_.columns() - 1; }
        if (dragHeader_ == 2) { anchor_.row = 0; caret_.row = table_.rows() - 1; settle(); }
        if (message == WM_LBUTTONDBLCLK && !dragHeader_) {
            // Place the text caret under the pointer, following the cell if
            // starting the edit scrolled it into view.
            // Hit-test the text unscrolled, as the cell showed it.
            const auto before = cellRect(caret_).left; startEdit();
            if (editingInCell()) {
                textCaret_ = textAnchor_ = 0; input_.reset(); inputChanged();
                textCaret_ = textAnchor_ = std::min(input_.hit(point.x + cellRect(caret_).left - before), text_.size()); inputChanged();
            }
            return true;
        }
        selecting_ = true; dragScrolling_ = dragLoading_ = false; dragPoint_ = point; dragCarryX_ = dragCarryY_ = 0; SetCapture(window_); return true;
    }
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        if (resizing_ >= 0) { widths_[resizing_] = std::clamp(resizeWidth_ + MulDiv(point.x - resizeX_, 96, dpi_), 40, 2048); layout(); InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_); }
        else if (inputSelecting_) { if (!ime_.active()) { textCaret_ = std::min(input_.hit(point.x), text_.size()); inputChanged(); } }
        else if (selecting_) drag(point);
        return true;
    }
    case WM_LBUTTONUP:
        if (selecting_) drag({GET_X_LPARAM(b), GET_Y_LPARAM(b)});
        stopDrag(); return true;
    case WM_CANCELMODE: case WM_CAPTURECHANGED: stopDrag(); return false;
    case WM_TIMER:
        if (a != statusTimer) return false;
        if (statusPending_) dragStatus(true);
        return true;
    case WM_MOUSEWHEEL:
        // The owner scrolls a large file's window and loads its neighbours.
        if (partial()) return false;
        wheel_ += GET_WHEEL_DELTA_WPARAM(a); top_ -= wheel_ / WHEEL_DELTA * 3; wheel_ %= WHEEL_DELTA;
        layout(); InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_); return true;
    case WM_VSCROLL: case WM_HSCROLL: {
        if (message == WM_VSCROLL && partial()) return false;
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
    case WM_SYSKEYDOWN:
        if (editing_ && a == VK_RETURN && !ime_.active()) { replaceInput(wide(edit_->newline())); return true; }
        return false;
    case WM_KEYDOWN: {
        if (ime_.active()) { result = DefWindowProcW(window_, message, a, b); return true; }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && a == 'S') { finishEdit(); if (save) save(); return true; }
        // An unparsable window can still move to the file's start or end.
        if (!valid_ && partial() && moveRows && ctrl && (a == VK_HOME || a == VK_END)) { moveRows(a == VK_HOME ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max(), false); return true; }
        if (!valid_) return true;
        if (ctrl && (a == 'Z' || a == 'Y')) { undo(a == 'Y' || shift); return true; }
        if (editing_ && inputKey(a, ctrl, shift)) return true;
        if (editing_) return true;
        if (ctrl && (a == 'C' || a == 'X')) { copy(a == 'X'); return true; }
        if ((ctrl && a == 'V') || (shift && a == VK_INSERT)) { paste(); return true; }
        if (ctrl && a == 'A') { anchor_ = {}; caret_ = {table_.rows() - 1, table_.columns() - 1}; settle(); InvalidateRect(window_, nullptr, FALSE); if (statusChanged) statusChanged(); return true; }
        if (a == VK_F2 || a == VK_BACK) { startEdit(a == VK_BACK); if (a == VK_BACK) replaceInput(L""); return true; }
        if (a == VK_DELETE) { clear(); return true; }
        if (a == VK_ESCAPE) { stopDrag(); return true; }
        if (a == VK_LEFT || a == VK_RIGHT || a == VK_UP || a == VK_DOWN || a == VK_HOME || a == VK_END || a == VK_PRIOR || a == VK_NEXT || a == VK_TAB || a == VK_RETURN) { move(a, shift, ctrl); return true; }
        return true;
    }
    case WM_CHAR:
        if (!ime_.active() && !(GetKeyState(VK_CONTROL) & 0x8000) && a >= 32 && a != 127 && edit_ && valid_) {
            const auto ch = static_cast<wchar_t>(a);
            if (!editing_) startEdit(true);
            if (ch >= 0xd800 && ch <= 0xdbff) { highSurrogate_ = ch; return true; }
            std::wstring text;
            if (highSurrogate_ && ch >= 0xdc00 && ch <= 0xdfff) text += highSurrogate_;
            highSurrogate_ = 0; text += ch; replaceInput(std::move(text));
        }
        return true;
    case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION:
        if (!edit_ || !valid_) return true;
        if (!editing_) startEdit(true);
        [[fallthrough]];
    case WM_IME_SETCONTEXT: case WM_INPUTLANGCHANGE: case WM_IME_NOTIFY: case WM_IME_ENDCOMPOSITION:
        result = ime_.handle(window_, message, a, b, [this] { inputChanged(); }, [this](std::wstring text) { replaceInput(std::move(text)); }); return true;
    case WM_CONTEXTMENU: {
        if (!valid_) return true;
        const bool editing = editing_;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Copy\tCtrl+C"); AppendMenuW(menu, MF_STRING, 2, L"Select all\tCtrl+A");
        if (edit_) {
            AppendMenuW(menu, MF_STRING, 3, L"Cut\tCtrl+X"); AppendMenuW(menu, MF_STRING, 4, L"Paste\tCtrl+V");
            AppendMenuW(menu, MF_STRING, 5, L"Undo\tCtrl+Z"); AppendMenuW(menu, MF_STRING, 6, L"Redo\tCtrl+Y");
            AppendMenuW(menu, MF_STRING, 7, L"Edit cell\tF2"); AppendMenuW(menu, MF_STRING, 8, L"Clear cells\tDelete"); AppendMenuW(menu, MF_STRING, 9, L"Save\tCtrl+S");
        }
        POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        if (point.x == -1) { const auto cell = cellRect(caret_); point = {cell.left, cell.bottom}; ClientToScreen(window_, &point); }
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu);
        if (editing && command >= 1 && command <= 4) inputKey(command == 1 ? 'C' : command == 2 ? 'A' : command == 3 ? 'X' : 'V', true, false);
        else if (command == 1) copy();
        else if (command == 2) { anchor_ = {}; caret_ = {table_.rows() - 1, table_.columns() - 1}; settle(); InvalidateRect(window_, nullptr, FALSE); }
        else if (edit_) {
            if (command == 3) copy(true); else if (command == 4) paste(); else if (command == 5 || command == 6) undo(command == 6);
            else if (command == 7) startEdit(); else if (command == 8) { finishEdit(); clear(); } else if (command == 9) { finishEdit(); if (save) save(); }
        }
        return true;
    }
    default: return false;
    }
}
}
