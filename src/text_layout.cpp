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
size_t wrappedLineRows(std::string_view line, int columns) {
    WrappedRow row; size_t count = 0;
    do {
        row = nextRow(line, row, std::max(1, columns)); ++count;
    } while (row.end < line.size() && row.endColumn < static_cast<int>(maxLineColumns));
    return count;
}
WrappedRow wrappedLineRow(std::string_view line, int columns, size_t continuation) {
    WrappedRow row;
    for (size_t i = 0;; ++i) {
        row = nextRow(line, row, std::max(1, columns));
        if (i == continuation) return row;
        if (row.end == line.size() || row.endColumn >= static_cast<int>(maxLineColumns)) {
            // A shorter split-diff side has blank padding after its last row.
            row.begin = row.end; row.column = row.endColumn; return row;
        }
    }
}
size_t wrappedLineRowForOffset(std::string_view line, int columns, size_t byte, bool trailing) {
    WrappedRow row;
    for (size_t i = 0;; ++i) {
        row = nextRow(line, row, std::max(1, columns));
        if (byte < row.end || (trailing && byte == row.end && row.end != row.begin) ||
            row.end == line.size() || row.endColumn >= static_cast<int>(maxLineColumns)) return i;
    }
}
void TextLayout::build(const Document& document, int columns) {
    columns_ = std::max(1, columns);
    starts_.clear(); starts_.reserve(document.lines.size() + 1); starts_.push_back(0);
    for (size_t lineNumber = 0; lineNumber < document.lines.size(); ++lineNumber) {
        starts_.push_back(starts_.back() + static_cast<uint32_t>(wrappedLineRows(document.line(lineNumber), columns_)));
    }
}
WrappedRow TextLayout::row(const Document& document, size_t visualRow) const {
    if (!rows()) return {};
    visualRow = std::min(visualRow, rows() - 1);
    const size_t lineNumber = static_cast<size_t>(std::upper_bound(starts_.begin(), starts_.end(), visualRow) - starts_.begin() - 1);
    auto row = wrappedLineRow(document.line(lineNumber), columns_, visualRow - starts_[lineNumber]);
    row.line = lineNumber; return row;
}
size_t TextLayout::rowForOffset(const Document& document, size_t offset, bool trailing) const {
    if (!rows()) return 0;
    offset = std::min(offset, document.text.size());
    const size_t lineNumber = static_cast<size_t>(std::upper_bound(document.lines.begin(), document.lines.end(), offset) - document.lines.begin() - 1);
    const size_t byte = offset - document.lines[lineNumber];
    return starts_[lineNumber] + wrappedLineRowForOffset(document.line(lineNumber), columns_, byte, trailing);
}
}
