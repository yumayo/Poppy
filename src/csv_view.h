#pragma once
#include "csv_search.h"
#include "edit_buffer.h"
#include "scrollbars.h"
#include "session.h"
#include <algorithm>
#include <chrono>
#include <limits>

namespace poppy {
// Uses the viewer's body window so focus, file drops and tab shortcuts keep
// working exactly as they do in the source editor.
class CsvView {
public:
    void attach(HWND window, Renderer& renderer, Scrollbars& scrollbars);
    void setDpi(UINT dpi);
    void show(std::shared_ptr<const Document> document, bool preserve);
    void buffer(std::shared_ptr<EditBuffer> buffer) { edit_ = std::move(buffer); }
    void hide(bool release = false);
    bool active() const { return active_; }
    bool composing() const { return ime_.active(); }
    bool editing() const { return editing_; }
    void refresh();
    void layout();
    void paint();
    bool handle(UINT message, WPARAM a, LPARAM b, LRESULT& result);
    ViewState state() const;
    void restore(const ViewState& state);
    std::wstring status() const;
    std::optional<std::wstring> searchSeed() const;
    size_t bytes() const { return table_.bytes() + widths_.capacity() * sizeof(int) + edges_.capacity() * sizeof(int) + text_.capacity() * sizeof(wchar_t); }
    // Cell search: start a scan, run it in slices, then select the target.
    void find(std::string query, bool matchCase, bool navigate, bool backwards);
    bool findStep(size_t cellBudget);
    void revealFind();
    void clearFind();
    const CsvSearch& search() const { return search_; }
    std::function<void()> changed, save, statusChanged;
    // Height reserved above the table, such as the viewer's find bar.
    std::function<int()> inset;
    // A large file's bounded window. The owner names rows by their text
    // offset, maps the vertical bar to file bytes, and loads neighbouring
    // windows when the caret leaves the loaded records. moveRows receives
    // INT64_MIN / INT64_MAX for the file's start / end and returns false when
    // the file has no further records in that direction.
    bool partial() const { return document_ && document_->tail; }
    std::function<std::wstring(size_t)> rowLabel;
    std::function<void(SCROLLINFO&, int rows, int page)> fileScroll;
    std::function<bool(int64_t rows, bool extend)> moveRows;
    // A drag that scrolls onto the window's top (-1) or bottom (1) edge asks
    // the owner once per window to load the records beyond it around the rows
    // on screen. The owner holds the drag while it shows that window, and the
    // drag scrolls on from there.
    std::function<void(int direction)> loadRows;
    bool dragging() const { return selecting_; }
    void holdDrag(bool hold);
    bool valid() const { return valid_; }
    bool truncated() const { return valid_ && table_.truncated(); }
    size_t rows() const { return valid_ ? table_.rows() : 0; }
    int top() const { return top_; }
    int left() const { return left_; }
    int page() const { return pageRows(); }
    bool atBottom() const { return !valid_ || top_ >= std::max(0, static_cast<int>(gridRows()) - pageRows()); }
    void scrollTo(int top, std::optional<int> left = std::nullopt);
    void scrollToBottom() { scrollTo(std::numeric_limits<int>::max() / 2); }
    // Text offsets of a row's first byte and of the selected cells.
    size_t rowOffset(size_t row) const { return table_.offset({row, 0}); }
    size_t rowAt(size_t offset) const { return valid_ ? table_.position(offset).row : 0; }
    size_t anchorOffset() const { return valid_ ? table_.offset(anchor_) : 0; }
    size_t caretOffset() const { return valid_ ? table_.offset(caret_) : 0; }
    size_t caretRow() const { return caret_.row; }
    size_t caretColumn() const { return caret_.column; }
    size_t anchorColumn() const { return anchor_.column; }
    // A large file's selected cell may lie outside the loaded window: -1
    // before it, 1 after it, 0 inside. The owner keeps such a cell's file
    // byte; the view keeps its column and shows its remembered name and value.
    struct Mark { int side = 0; size_t offset = 0, column = 0; };
    int anchorSide() const { return anchorSide_; }
    int caretSide() const { return caretSide_; }
    std::wstring caretName() const { return caretSide_ ? caretName_ : rowName(caret_.row); }
    std::wstring caretValue() const;
    void selectMarks(Mark anchor, Mark caret, std::wstring caretName = {}, std::wstring caretValue = {});
    void moveCaret(size_t row, size_t column, bool extend);
private:
    int insetTop() const { return inset ? inset() : 0; }
    int headerTop() const { return insetTop() + px(34); }
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    int rowHeight() const { return std::max(px(28), renderer_->textHeight() + px(8)); }
    int bodyTop() const { return headerTop() + rowHeight(); }
    int rowGutter() const;
    int nameWidth() const;
    std::wstring rowName(size_t row) const;
    int pageRows() const;
    // An editable table adds an input row and column after its data.
    size_t gridRows() const { return std::min(CsvTable::maxCells, table_.rows() + (partial() ? 0 : 1)); }
    size_t gridColumns() const { return std::min(CsvTable::maxColumns, table_.columns() + (partial() ? 0 : 1)); }
    RECT formula() const;
    RECT cellRect(CsvPosition cell) const;
    CsvPosition hit(POINT point) const;
    CsvRange selection() const;
    // Whether the selection covers any loaded row, which it does not when both
    // of its ends lie beyond the same edge of a large file's window.
    bool selectionLoaded() const { return !(anchorSide_ < 0 && caretSide_ < 0) && !(anchorSide_ > 0 && caretSide_ > 0); }
    void settle() { anchorSide_ = caretSide_ = 0; }
    // A large file's cells outside its window are drawn at the window's edge
    // and are not loaded, so only a selection inside the window is edited.
    bool editable() const { return edit_ && valid_ && !anchorSide_ && !caretSide_; }
    // Whether rows may be added after a large file's window, which they may
    // only at the end of the file.
    bool appendable() const;
    int resizeColumn(POINT point) const;
    void rebuild();
    void reveal();
    void select(CsvPosition cell, bool extend = false, bool scrollIntoView = true);
    void drag(POINT point);
    void stopDrag();
    void dragCaret();
    bool advanceDrag();
    void dragStatus(bool flush = false);
    void move(WPARAM key, bool shift, bool ctrl);
    void copy(bool cut = false);
    void paste();
    void clear();
    bool apply(const std::optional<CsvReplacement>& replacement, bool join = false);
    void undo(bool redo);
    void startEdit(bool empty = false);
    void finishEdit(bool cancel = false);
    void inputChanged();
    void replaceInput(std::wstring text);
    size_t previous(size_t offset) const;
    size_t next(size_t offset) const;
    bool inputKey(WPARAM key, bool ctrl, bool shift);
    HWND window_ = nullptr;
    Renderer* renderer_ = nullptr;
    Scrollbars* scrollbars_ = nullptr;
    std::shared_ptr<const Document> document_;
    std::shared_ptr<EditBuffer> edit_;
    CsvTable table_;
    CsvSearch search_;
    UINT dpi_ = 96;
    bool active_ = false, valid_ = false, editing_ = false, editChanged_ = false, selecting_ = false, inputSelecting_ = false;
    // Column widths use 96-DPI units; the scroll range uses physical pixels.
    std::vector<int> widths_, edges_;
    int top_ = 0, left_ = 0, resizing_ = -1, resizeX_ = 0, resizeWidth_ = 0, wheel_ = 0;
    CsvPosition anchor_, caret_;
    int anchorSide_ = 0, caretSide_ = 0;
    std::wstring caretName_, caretValue_;
    POINT dragPoint_{};
    int dragHeader_ = 0;
    std::chrono::steady_clock::time_point dragTick_;
    double dragCarryX_ = 0, dragCarryY_ = 0;
    bool dragScrolling_ = false, statusPending_ = false, dragHeld_ = false, dragLoading_ = false;
    // The status bar repaints its own windows, each waiting for vertical sync,
    // so a drag reports at most this often instead of on every frame.
    std::chrono::steady_clock::time_point statusTick_;
    static constexpr UINT_PTR statusTimer = 0x5062;
    InputLine input_;
    ImeComposition ime_;
    std::wstring text_;
    size_t textAnchor_ = 0, textCaret_ = 0;
    wchar_t highSurrogate_ = 0;
    uint64_t editRevision_ = 0;
};
}
