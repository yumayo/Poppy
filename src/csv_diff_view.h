#pragma once
#include "csv_diff.h"
#include "scrollbars.h"
#include <functional>
#include <memory>

namespace poppy {
// A read-only comparison of a CSV file's two versions as tables side by
// side. Both tables scroll together; their aligned records share a row, and
// a record missing on one side leaves an empty row there. Like CsvView, it
// draws into the viewer's body window.
class CsvDiffView {
public:
    void attach(HWND window, Renderer& renderer, Scrollbars& scrollbars) { window_ = window; renderer_ = &renderer; scrollbars_ = &scrollbars; }
    void setDpi(UINT dpi) { dpi_ = dpi; if (active_) layout(); }
    // A reload of the same file keeps the scroll position and selected cell.
    void show(std::shared_ptr<const CsvComparison> comparison, bool preserve);
    void hide(bool release = false);
    bool dragging() const { return selecting_; }
    bool active() const { return active_; }
    void layout();
    void paint();
    bool handle(UINT message, WPARAM a, LPARAM b, LRESULT& result);
    std::wstring status() const;
    size_t bytes() const { return comparison_ ? comparison_->bytes() + changes_.capacity() * sizeof(Change) : 0; }
    std::function<void()> statusChanged;
    // Height reserved above the table, such as the viewer's find bar.
    std::function<int()> inset;
private:
    struct Change { size_t begin = 0, end = 0; bool removed = false; };
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    int insetTop() const { return inset ? inset() : 0; }
    int headerTop() const { return insetTop() + px(34); }
    int rowHeight() const { return std::max(px(28), renderer_->textHeight() + px(8)); }
    int bodyTop() const { return headerTop() + rowHeight(); }
    int columnWidth() const { return px(160); }
    int gutter() const;
    // Each side's table, excluding the change marks strip on the right.
    RECT half(bool after) const;
    int pageRows() const;
    size_t rows() const { return comparison_ && comparison_->valid() ? comparison_->rows.size() : 0; }
    size_t columns() const { return comparison_ && comparison_->valid() ? std::max<size_t>(1, comparison_->columns) : 0; }
    bool hit(POINT point, size_t& row, size_t& column, bool& after) const;
    // The cell under a point on one side, clamped to that side's cells, for
    // a drag that leaves the table.
    void hitSide(POINT point, bool after, size_t& row, size_t& column) const;
    struct Range { size_t firstRow, firstColumn, lastRow, lastColumn; };
    Range selection() const;
    void select(size_t row, size_t column, bool extend = false);
    void drag(POINT point);
    void stopDrag();
    void reveal();
    void move(WPARAM key, bool shift, bool ctrl);
    void nextChange(bool backwards);
    void copy();
    void drawSide(bool after);
    void drawMarks();
    HWND window_ = nullptr;
    Renderer* renderer_ = nullptr;
    Scrollbars* scrollbars_ = nullptr;
    std::shared_ptr<const CsvComparison> comparison_;
    std::vector<Change> changes_;
    UINT dpi_ = 96;
    bool active_ = false, after_ = true;
    int top_ = 0, left_ = 0, wheel_ = 0;
    // The caret and the range's other corner, as aligned rows and columns.
    size_t row_ = 0, column_ = 0, anchorRow_ = 0, anchorColumn_ = 0;
    bool selecting_ = false;
    // A drag that started on the row numbers or column names selects whole
    // rows (1) or columns (2).
    int dragHeader_ = 0;
    POINT dragPoint_{};
    static constexpr UINT_PTR dragTimer = 0x5064;
};
}
