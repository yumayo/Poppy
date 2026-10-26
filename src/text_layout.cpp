#include "text_layout.h"
#include <algorithm>

namespace poppy {
namespace {
WrappedRow nextRow(std::string_view line, WrappedRow row, int columns) {
    row.begin = row.end;
    row.column = row.endColumn;
    int column = row.column;
    while (row.end < line.size() && column < static_cast<int>(maxLineColumns)) {
        size_t next = row.end;
        const auto ch = decodeUtf8(line, next);
        const int width = ch == '\t' ? 4 - column % 4 : std::max(1, cellWidth(ch));
        if (column + width - row.column > columns && row.end != row.begin) break;
        row.end = next; column += width;
    }
    row.endColumn = column;
    return row;
}
}
void TextLayout::build(const Document& document, int columns) {
    columns_ = std::max(1, columns);
    starts_.clear(); starts_.reserve(document.lines.size() + 1); starts_.push_back(0);
    for (size_t lineNumber = 0; lineNumber < document.lines.size(); ++lineNumber) {
        const auto line = document.line(lineNumber);
        WrappedRow row; uint32_t count = 0;
        do {
            row = nextRow(line, row, columns_); ++count;
        } while (row.end < line.size() && row.endColumn < static_cast<int>(maxLineColumns));
        starts_.push_back(starts_.back() + count);
    }
}
WrappedRow TextLayout::row(const Document& document, size_t visualRow) const {
    if (!rows()) return {};
    visualRow = std::min(visualRow, rows() - 1);
    const size_t lineNumber = static_cast<size_t>(std::upper_bound(starts_.begin(), starts_.end(), visualRow) - starts_.begin() - 1);
    const auto line = document.line(lineNumber);
    WrappedRow row; row.line = lineNumber;
    for (size_t i = starts_[lineNumber];; ++i) {
        row = nextRow(line, row, columns_);
        if (i == visualRow) return row;
    }
}
size_t TextLayout::rowForOffset(const Document& document, size_t offset, bool trailing) const {
    if (!rows()) return 0;
    offset = std::min(offset, document.text.size());
    const size_t lineNumber = static_cast<size_t>(std::upper_bound(document.lines.begin(), document.lines.end(), offset) - document.lines.begin() - 1);
    const auto line = document.line(lineNumber);
    const size_t byte = offset - document.lines[lineNumber];
    WrappedRow row;
    for (size_t i = starts_[lineNumber];; ++i) {
        row = nextRow(line, row, columns_);
        if (byte < row.end || (trailing && byte == row.end && row.end != row.begin) || i + 1 == starts_[lineNumber + 1]) return i;
    }
}
}
