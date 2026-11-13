#include "terminal_display.h"
#include "terminal_link.h"
#include <algorithm>

namespace poppy {
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
    dirty_ = false;
    return true;
}
CompositionLayout TerminalDisplay::echoedComposition(const CompositionText& text) const {
    if (text.text.empty() || scroll_ || cursorY_ < 0 || cursorY_ >= rows() || columns_ < 2) return {};
    // A single unbounded row gives each glyph's character, width and attribute.
    auto glyphs = text.layout(1 << 20, 0);
    int y = cursorY_, x = wrapPending_ ? columns_ : std::min(cursorX_, columns_);
    for (auto glyph = glyphs.glyphs.rbegin(); glyph != glyphs.glyphs.rend(); ++glyph) {
        const int width = glyph->width;
        if (x == 0) {
            // Continue only across an automatic wrap, excluding wide-glyph padding.
            if (y == 0 || !wrapped_[y - 1]) return {};
            x = wrapped_[--y];
        }
        if (x < width) return {};
        const auto& cells = row(y);
        if (cells[x - width].continuation || cells[x - width].ch != glyph->character ||
            (width == 2 && !cells[x - 1].continuation)) return {};
        x -= width;
        glyph->row = y; glyph->column = x;
    }
    CompositionLayout result;
    result.glyphs = std::move(glyphs.glyphs);
    result.endRow = cursorY_; result.endColumn = wrapPending_ ? columns_ : cursorX_;
    // The IME caret sits before the glyph at its offset, or at the cursor.
    int offset = glyphs.caretColumn;
    result.caretRow = result.endRow; result.caretColumn = result.endColumn;
    for (const auto& glyph : result.glyphs) {
        if (offset <= 0) { result.caretRow = glyph.row; result.caretColumn = glyph.column; break; }
        offset -= glyph.width;
    }
    return result;
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
