#include "terminal_display.h"
#include "terminal_link.h"
#include <algorithm>

namespace poppy {
namespace {
bool verticalBorder(char32_t ch) {
    return ch == U'│' || ch == U'┃' || ch == U'┆' || ch == U'┇' || ch == U'┊' || ch == U'┋' ||
        ch == U'╎' || ch == U'╏' || ch == U'║' || (ch >= U'├' && ch <= U'┫');
}
bool horizontalBorder(const std::vector<Cell>& row) {
    int strokes = 0;
    for (const auto& cell : row) {
        const auto ch = cell.ch;
        if (ch == U'-' || ch == U'_' || ch == U'=' || ch == U'─' || ch == U'━' ||
            (ch >= U'┄' && ch <= U'┅') || (ch >= U'┈' && ch <= U'┉') || ch == U'╌' || ch == U'╍' || ch == U'═') ++strokes;
        else if (ch != U' ' && !(ch >= U'┌' && ch <= U'╋') && !(ch >= U'╔' && ch <= U'╬') &&
            !(ch >= U'╭' && ch <= U'╰') && !verticalBorder(ch)) return false;
    }
    return strokes >= 3;
}
bool emptyRow(const std::vector<Cell>& row) {
    return std::all_of(row.begin(), row.end(), [](const Cell& cell) {
        return cell.ch == U' ' && !cell.continuation && cell.bg == background;
    });
}
}
bool TerminalDisplay::update(TerminalModel& model, size_t scroll, uint64_t now, bool force) {
    scroll = std::min(scroll, model.historySize());
    const bool scrollChanged = scroll != scroll_;
    if (model.synchronizedOutput()) {
        if (!synchronizing_) { synchronizing_ = true; syncSince_ = now; }
        // A missing end marker must not leave the display frozen indefinitely.
        if (!force && now - syncSince_ < 1000) return false;
        model.endSynchronizedOutput();
        force = true;
    }
    synchronizing_ = false;
    if (!dirty_ && !force && !scrollChanged) return false;

    if (generation_ != model.generation() || columns_ != model.columns() || rows() != model.rows()) cancelPreedit();
    if (generation_ != model.generation() ||
        (anchor_ >= 0 && std::min(anchor_, caret_) < model.firstRow() * model.columns())) clearSelection();
    generation_ = model.generation();
    firstRow_ = model.firstRow() + static_cast<int64_t>(model.historySize() - scroll);
    columns_ = model.columns(); scroll_ = scroll;
    cells_.resize(static_cast<size_t>(model.rows()));
    wrapped_.resize(cells_.size());
    for (int y = 0; y < model.rows(); ++y) {
        cells_[y] = model.row(y, scroll_); cells_[y].resize(static_cast<size_t>(columns_));
        wrapped_[y] = model.wrappedLength(y, scroll_);
    }
    continuesAbove_ = scroll_ < model.historySize() && model.wrappedLength(0, scroll_ + 1) != 0;
    cursorX_ = model.cursorX(); cursorY_ = model.cursorY(); cursorVisible_ = model.cursorVisible();
    wrapPending_ = model.wrapPending();
    if (pendingCommit_ && commitPresented()) { pendingCommit_.reset(); endPreedit(); }
    dirty_ = false;
    return true;
}
std::string TerminalDisplay::linkAt(int column, int y) const {
    if (column < 0 || column >= columns_ || y < 0 || y >= rows()) return {};
    int first = y, last = y;
    while (first > 0 && wrapped_[first - 1]) --first;
    while (last + 1 < rows() && wrapped_[last]) ++last;
    // A partially visible logical line must not open a partial destination.
    if ((first == 0 && continuesAbove_) || wrapped_[last]) return {};
    std::u32string text;
    size_t position = 0;
    for (int rowIndex = first; rowIndex <= last; ++rowIndex) {
        const int length = wrapped_[rowIndex] ? wrapped_[rowIndex] : columns_;
        if (rowIndex == y) {
            if (column >= length) return {};
            position = text.size() + column;
        }
        for (int x = 0; x < length; ++x) text += row(rowIndex)[x].continuation ? U' ' : row(rowIndex)[x].ch;
    }
    return terminalLinkAt(text, position);
}
void TerminalDisplay::beginPreedit() {
    if (preeditCursor_ || scroll_ || !rows()) return;
    // While echo is pending, retain the last input location rather than an
    // unrelated redraw cursor. update() releases it once the new input lands.
    if (pendingCommit_ && pendingCommit_->row >= firstRow_ && pendingCommit_->row < firstRow_ + rows())
        preeditCursor_ = Cursor{std::min(pendingCommit_->column, columns_ - 1),
            static_cast<int>(pendingCommit_->row - firstRow_), pendingCommit_->column == columns_};
    else preeditCursor_ = Cursor{cursorX_, cursorY_, wrapPending_};
}
void TerminalDisplay::inputSent(std::u16string text) {
    // Editing commands and line submission invalidate the expected echo. Bound
    // tracking to a screenful even for non-echoing or reformatting applications.
    if (std::any_of(text.begin(), text.end(), [](char16_t ch) { return ch < 32 || ch == 127; }) ||
        text.size() + (pendingCommit_ ? pendingCommit_->text.size() : 0) > static_cast<size_t>(columns_ * rows() * 2)) {
        cancelPreedit(); return;
    }
    if (!text.empty() && !scroll_ && rows()) {
        if (pendingCommit_) pendingCommit_->text += text;
        else {
            const int y = inputCursorY();
            int column = (preeditCursor_ ? preeditCursor_->wrap : wrapPending_) ? columns_ : inputCursorX();
            if (column < columns_ && row(y)[column].continuation) --column;
            std::u32string prefix;
            for (int x = 0; x < column; ++x) prefix += row(y)[x].ch;
            pendingCommit_ = Commit{std::move(text), column, firstRow_ + y, std::move(prefix)};
        }
    }
    endPreedit();
}
bool TerminalDisplay::commitPresented() const {
    if (scroll_) return false;
    const auto& pending = *pendingCommit_;
    const auto layout = CompositionText{pending.text, {}, pending.text.size()}.layout(columns_, pending.column);
    // Output can arrive in multiple chunks or redraw unrelated rows first. A
    // changed cursor alone is not evidence that the input has been processed.
    if (layout.endColumn != (wrapPending_ ? 0 : cursorX_)) return false;
    // A growing TUI input area may move the whole prompt vertically. Match the
    // echo and its original prefix at the new cursor, not only at the old row.
    const int first = cursorY_ + (wrapPending_ ? 1 : 0) - layout.endRow;
    if (first >= 0) for (size_t x = 0; x < pending.prefix.size(); ++x)
        if (row(first)[x].ch != pending.prefix[x]) return false;
    bool visible = false;
    for (const auto& glyph : layout.glyphs) {
        const int y = first + glyph.row;
        if (y < 0) continue; // Echo may have scrolled the beginning into history.
        if (y >= rows() || row(static_cast<int>(y))[glyph.column].ch != glyph.character) return false;
        visible = true;
    }
    return visible;
}
TerminalPreedit TerminalDisplay::preedit(const CompositionText& text) const {
    TerminalPreedit result;
    const int cursorY = inputCursorY();
    if (text.text.empty() || scroll_ || columns_ < 2 || cursorY < 0 || cursorY >= rows() || horizontalBorder(row(cursorY))) return result;

    int column = (preeditCursor_ ? preeditCursor_->wrap : wrapPending_) ? columns_ : inputCursorX();
    // A cursor-addressing sequence may land inside a wide glyph.
    if (column < columns_ && row(cursorY)[column].continuation) --column;
    int left = 0, right = columns_;
    for (int x = 0; x < column; ++x) if (verticalBorder(row(cursorY)[x].ch)) left = x + 1;
    for (int x = column; x < columns_; ++x) if (verticalBorder(row(cursorY)[x].ch)) { right = x; break; }
    if (right - left < 2) return result;
    const bool framed = left > 0 || right < columns_;
    int first = cursorY, last = cursorY;
    if (!framed) {
        while (first > 0 && wrapped_[first - 1] && !horizontalBorder(row(first - 1))) --first;
        while (last + 1 < rows() && wrapped_[last] && !horizontalBorder(row(last + 1))) ++last;
    }
    int available = last;
    // Only automatic continuations and unused rows belong to this preview.
    // Hard line breaks, colored panels and box borders remain in place.
    if (!framed) while (available + 1 < rows() && emptyRow(row(available + 1))) ++available;

    const int width = right - left;
    auto layout = text.layout(width, (cursorY - first) * width + column - left);
    const int offset = std::max(0, layout.caretRow - (available - first));
    result.cells = cells_;
    result.left = left; result.right = right; result.firstRow = first; result.lastRow = available;
    result.compositionBackground = row(cursorY)[std::min(column, right - 1)].bg;
    for (int y = first; y <= available; ++y) {
        Cell blank; blank.bg = row(y)[left].bg;
        std::fill(result.cells[y].begin() + left, result.cells[y].begin() + right, blank);
    }

    // Retain the input before the insertion point. When space is exhausted,
    // pan only this logical input block to keep the IME cursor visible.
    for (int y = first; y <= cursorY; ++y) {
        const int destination = y - offset;
        if (destination < first || destination > available) continue;
        const int end = y == cursorY ? column : (wrapped_[y] ? wrapped_[y] : right);
        std::copy(row(y).begin() + left, row(y).begin() + end, result.cells[destination].begin() + left);
    }

    // Shift whole glyphs after the *end* of preedit, independently of the IME
    // cursor or the active conversion clause. Preserve terminal colors.
    int targetRow = first + layout.endRow - offset, targetColumn = left + layout.endColumn;
    for (int y = cursorY; y <= last; ++y) {
        int end = y < last ? wrapped_[y] : right;
        if (y == last) while (end > left && row(y)[end - 1].ch == U' ' && !row(y)[end - 1].continuation) --end;
        for (int x = y == cursorY ? column : left; x < end; ++x) {
            const auto& cell = row(y)[x];
            if (cell.continuation) continue;
            const int advance = std::max(1, cellWidth(cell.ch));
            if (targetColumn + advance > right) { ++targetRow; targetColumn = left; }
            if (targetRow >= first && targetRow <= available) {
                result.cells[targetRow][targetColumn] = cell;
                if (advance == 2) {
                    auto tail = cell; tail.ch = 0; tail.continuation = true;
                    result.cells[targetRow][targetColumn + 1] = tail;
                }
            }
            targetColumn += advance;
        }
    }
    for (auto& glyph : layout.glyphs) { glyph.row += first - offset; glyph.column += left; }
    layout.caretRow += first - offset; layout.caretColumn += left;
    layout.endRow += first - offset; layout.endColumn += left;
    result.composition = std::move(layout);
    return result;
}
std::pair<int, int> TerminalDisplay::selectionRange(int anchor, int caret) const {
    if (anchor < 0 || caret < 0 || !columns_) return {0, 0};
    const int limit = columns_ * rows();
    int begin = std::clamp(std::min(anchor, caret), 0, limit);
    int end = std::clamp(std::max(anchor, caret), 0, limit);
    if (begin == end) return {begin, end};
    // Include a whole wide character when either boundary cuts its cells.
    if (row(begin / columns_)[begin % columns_].continuation) --begin;
    if (end < limit && row(end / columns_)[end % columns_].continuation) ++end;
    return {begin, end};
}
std::string TerminalDisplay::selectedText(int anchor, int caret) const {
    const auto [begin, end] = selectionRange(anchor, caret);
    std::string result;
    for (int position = begin; position < end;) {
        const int y = position / columns_, rowEnd = std::min(end, (y + 1) * columns_);
        std::string part;
        for (; position < rowEnd; ++position) {
            const auto& cell = row(y)[position % columns_];
            if (!cell.continuation) appendUtf8(part, cell.ch);
        }
        while (!part.empty() && part.back() == ' ') part.pop_back();
        result += part;
        if (position < end) result += "\r\n";
    }
    return result;
}
void TerminalDisplay::beginSelection(int point) {
    clearSelection();
    if (columns_) anchor_ = caret_ = firstRow_ * columns_ + std::clamp(point, 0, columns_ * rows());
}
void TerminalDisplay::extendSelection(const TerminalModel& model, int point) {
    if (anchor_ < 0 || !columns_) return;
    const int64_t caret = firstRow_ * columns_ + std::clamp(point, 0, columns_ * rows());
    if (caret == anchor_) { caret_ = caret; selectionText_.clear(); return; }
    const int64_t begin = std::min(anchor_, caret), end = std::max(anchor_, caret);
    const int64_t first = begin / columns_, last = (end - 1) / columns_;
    // Only materialize the selected rows while dragging. Scrolling and output
    // do not copy the history or replace the text the user already selected.
    TerminalDisplay selection;
    selection.columns_ = columns_;
    for (int64_t y = first; y <= last; ++y) {
        if (y >= firstRow_ && y < firstRow_ + rows()) {
            selection.cells_.push_back(row(static_cast<int>(y - firstRow_)));
        } else {
            // The model may have advanced while a synchronized frame is held.
            // Do not mix buffers or dereference history that no longer exists.
            if (generation_ != model.generation() || y < model.firstRow() ||
                y >= model.firstRow() + static_cast<int64_t>(model.historySize()) + model.rows()) return;
            selection.cells_.push_back(model.row(static_cast<int>(y - model.firstRow()), model.historySize()));
            selection.cells_.back().resize(static_cast<size_t>(columns_));
        }
    }
    selectionText_ = selection.selectedText(static_cast<int>(begin - first * columns_), static_cast<int>(end - first * columns_));
    caret_ = caret;
}
std::pair<int, int> TerminalDisplay::selectionRange() const {
    if (!hasSelection()) return {0, 0};
    const int64_t origin = firstRow_ * columns_, limit = columns_ * rows();
    return selectionRange(static_cast<int>(std::clamp<int64_t>(anchor_ - origin, 0, limit)),
        static_cast<int>(std::clamp<int64_t>(caret_ - origin, 0, limit)));
}
}
