#include "diff_layout.h"
#include <algorithm>

namespace poppy {
void DiffLayout::build(const DiffDocument& document, bool inlineView, int columns) {
    columns_ = std::max(1, columns); inlineView_ = inlineView;
    starts_.clear(); starts_.reserve(document.rowCount(inlineView) + 1); starts_.push_back(0);
    for (size_t i = 0; i < document.rowCount(inlineView); ++i) {
        const auto change = document.displayRow(i, inlineView);
        const size_t count = change.heading ? 1 : std::max(wrappedLineRows(document.line(change.before), columns_), wrappedLineRows(document.line(change.after), columns_));
        starts_.push_back(starts_.back() + static_cast<uint32_t>(count));
    }
}
WrappedDiffRow DiffLayout::row(size_t visualRow) const {
    if (!rows()) return {};
    visualRow = std::min(visualRow, rows() - 1);
    const size_t logicalRow = static_cast<size_t>(std::upper_bound(starts_.begin(), starts_.end(), visualRow) - starts_.begin() - 1);
    return {logicalRow, visualRow - starts_[logicalRow]};
}
WrappedRow DiffLayout::textRow(const DiffDocument& document, size_t visualRow, bool after) const {
    if (!rows()) return {};
    const auto display = row(visualRow);
    const auto change = document.displayRow(display.row, inlineView_);
    auto result = wrappedLineRow(document.line(after ? change.after : change.before), columns_, display.continuation);
    result.line = display.row; return result;
}
size_t DiffLayout::rowForOffset(const DiffDocument& document, size_t logicalRow, size_t byte, bool after) const {
    if (!rows()) return 0;
    logicalRow = std::min(logicalRow, starts_.size() - 2);
    const auto change = document.displayRow(logicalRow, inlineView_);
    return starts_[logicalRow] + (change.heading ? 0 : wrappedLineRowForOffset(document.line(after ? change.after : change.before), columns_, byte));
}
}
