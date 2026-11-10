#pragma once
#include "edit_buffer.h"
#include "text_layout.h"
#include <optional>

namespace poppy {
// Offsets are UTF-8 boundaries. Virtual cells beyond EOL do not change text
// until something is inserted; wrapped rows keep their own visual columns.
struct ColumnRange {
    size_t begin = 0, end = 0;
    int padding = 0, virtualWidth = 0;
    bool trailing = false;
};
ColumnRange columnRange(std::string_view line, WrappedRow row, int first, int last);
std::optional<std::string> selectedColumns(std::string_view text, const std::vector<ColumnRange>& ranges, std::string_view newline = "\r\n");
enum class ColumnErase { None, Backward, Forward, WordBackward };
// Apply all rows atomically as one Undo operation. A paste with the same number
// of lines is distributed across rows; ordinary typing is repeated per cursor.
bool replaceColumns(EditBuffer& buffer, std::vector<ColumnRange>& ranges, std::string_view text,
    bool distribute = false, ColumnErase erase = ColumnErase::None);
}
