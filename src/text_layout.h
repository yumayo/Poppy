#pragma once
#include "text.h"

namespace poppy {
// Byte boundaries stay in the logical line. Tabs retain their logical columns.
struct WrappedRow {
    size_t line = 0, begin = 0, end = 0;
    int column = 0, endColumn = 0;
};
// Shared cell/UTF-8 wrapping for document lines and diff source lines.
size_t wrappedLineRows(std::string_view line, int columns);
WrappedRow wrappedLineRow(std::string_view line, int columns, size_t continuation);
size_t wrappedLineRowForOffset(std::string_view line, int columns, size_t byte, bool trailing = false);
class TextLayout {
public:
    void build(const Document& document, int columns);
    int columns() const { return columns_; }
    size_t rows() const { return starts_.empty() ? 0 : starts_.back(); }
    size_t bytes() const { return starts_.capacity() * sizeof(uint32_t); }
    WrappedRow row(const Document& document, size_t visualRow) const;
    size_t rowForOffset(const Document& document, size_t offset, bool trailing = false) const;
private:
    // One cumulative count per logical line, rather than per wrapped row.
    std::vector<uint32_t> starts_;
    int columns_ = 0;
};
}
