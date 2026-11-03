#pragma once
#include "text.h"
#include <algorithm>

namespace poppy {
// Byte boundaries stay in the logical line. Tabs retain their logical columns.
struct WrappedRow {
    size_t line = 0, begin = 0, end = 0;
    int column = 0, endColumn = 0;
};
// Shared cell/UTF-8 wrapping for document lines and diff source lines.
size_t wrappedLineRows(std::string_view line, int columns, int columnLimit = maxLineColumns);
WrappedRow wrappedLineRow(std::string_view line, int columns, size_t continuation, int columnLimit = maxLineColumns);
size_t wrappedLineRowForOffset(std::string_view line, int columns, size_t byte, bool trailing = false, int columnLimit = maxLineColumns);
class TextLayout {
public:
    void build(const Document& document, int columns, int columnLimit = maxLineColumns);
    int columns() const { return columns_; }
    size_t rows() const { return starts_.empty() ? 0 : starts_.back(); }
    // Accept the end boundary as well as source lines (for overview ranges).
    size_t lineStart(size_t line) const { return starts_.empty() ? 0 : starts_[std::min(line, starts_.size() - 1)]; }
    size_t bytes() const { return starts_.capacity() * sizeof(uint32_t); }
    WrappedRow row(const Document& document, size_t visualRow) const;
    size_t rowForOffset(const Document& document, size_t offset, bool trailing = false) const;
private:
    // One cumulative count per logical line, rather than per wrapped row.
    std::vector<uint32_t> starts_;
    int columns_ = 0;
    int columnLimit_ = maxLineColumns;
};
}
