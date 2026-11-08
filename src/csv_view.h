#pragma once
#include "csv_table.h"
#include "edit_buffer.h"
#include "scrollbars.h"
#include "session.h"
#include <algorithm>

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
    void refresh();
    void layout();
    void paint();
    bool handle(UINT message, WPARAM a, LPARAM b, LRESULT& result);
    ViewState state() const;
    void restore(const ViewState& state);
    std::wstring status() const;
    std::optional<std::wstring> searchSeed() const;
    size_t bytes() const { return table_.bytes() + widths_.capacity() * sizeof(int) + edges_.capacity() * sizeof(int) + text_.capacity() * sizeof(wchar_t); }
    std::function<void()> changed, save, statusChanged;
private:
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    int rowHeight() const { return std::max(px(28), renderer_->textHeight() + px(8)); }
    int bodyTop() const { return px(34) + rowHeight(); }
    int rowGutter() const;
    int pageRows() const;
    size_t gridRows() const { return std::min(CsvTable::maxCells, table_.rows() + 1); }
    size_t gridColumns() const { return std::min(CsvTable::maxColumns, table_.columns() + 1); }
    RECT formula() const;
    RECT cellRect(CsvPosition cell) const;
    CsvPosition hit(POINT point) const;
    CsvRange selection() const;
    int resizeColumn(POINT point) const;
    void rebuild();
    void reveal();
    void select(CsvPosition cell, bool extend = false, bool scrollIntoView = true);
    void drag(POINT point);
    void stopDrag();
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
    UINT dpi_ = 96;
    bool active_ = false, valid_ = false, editing_ = false, editChanged_ = false, selecting_ = false, inputSelecting_ = false;
    // Column widths use 96-DPI units; the scroll range uses physical pixels.
    std::vector<int> widths_, edges_;
    int top_ = 0, left_ = 0, resizing_ = -1, resizeX_ = 0, resizeWidth_ = 0, wheel_ = 0;
    CsvPosition anchor_, caret_;
    POINT dragPoint_{};
    int dragHeader_ = 0;
    InputLine input_;
    ImeComposition ime_;
    std::wstring text_;
    size_t textAnchor_ = 0, textCaret_ = 0;
    wchar_t highSurrogate_ = 0;
    uint64_t editRevision_ = 0;
    static constexpr UINT_PTR dragTimer = 0x5061;
};
}
