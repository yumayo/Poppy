#include "column_selection.h"
#include <algorithm>

namespace poppy {
ColumnRange columnRange(std::string_view line, WrappedRow row, int first, int last) {
    const int left = std::clamp(std::min(first, last), 0, static_cast<int>(maxLineColumns));
    const int right = std::clamp(std::max(first, last), 0, static_cast<int>(maxLineColumns));
    ColumnRange range{row.end, row.end};
    int column = row.column;
    bool found = false;
    for (size_t p = row.begin; p < row.end;) {
        const size_t begin = p; const auto c = decodeUtf8(line, p);
        const int width = c == '\t' ? 4 - column % 4 : std::max(1, cellWidth(c));
        if (!found && column + width - row.column > left) { range.begin = range.end = begin; found = true; }
        if (column - row.column < right && column + width - row.column > left && left != right) range.end = p;
        column += width;
    }
    const int length = column - row.column;
    range.padding = std::max(0, left - length);
    range.virtualWidth = std::max(0, right - std::max(left, length));
    range.trailing = range.end == row.end && row.end < line.size();
    return range;
}
std::optional<std::string> selectedColumns(std::string_view text, const std::vector<ColumnRange>& ranges, std::string_view newline) {
    std::string result;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        if (range.begin > range.end || range.end > text.size() || range.virtualWidth < 0) return {};
        const size_t length = range.end - range.begin + range.virtualWidth + (i ? newline.size() : 0);
        if (length > maxFileBytes - result.size()) return {};
        if (i) result += newline;
        result.append(text.substr(range.begin, range.end - range.begin));
        result.append(static_cast<size_t>(range.virtualWidth), ' ');
    }
    return result;
}
bool replaceColumns(EditBuffer& buffer, std::vector<ColumnRange>& ranges, std::string_view text, bool distribute, ColumnErase erase) {
    if (ranges.empty()) return false;
    auto selections = ranges;
    const auto& document = buffer.document;
    size_t previousEnd = 0;
    for (auto& range : selections) {
        if (range.begin > range.end || range.end > document.text.size() || range.begin < previousEnd || range.padding < 0 || range.virtualWidth < 0) return false;
        previousEnd = range.end;
        if (erase != ColumnErase::None && range.begin == range.end && !range.virtualWidth) {
            if (range.padding) { if (erase != ColumnErase::Forward) --range.padding; }
            else {
                if (erase == ColumnErase::Forward) range.end = buffer.next(range.end);
                else range.begin = erase == ColumnErase::WordBackward ? buffer.previousWord(range.begin) : buffer.previous(range.begin);
            }
        }
    }
    // Deletion at nearby wrapped cursors can overlap. Delete each byte once.
    if (erase != ColumnErase::None) {
        size_t count = 0;
        for (const auto& range : selections) {
            if (count && range.begin <= selections[count - 1].end && !range.padding && !selections[count - 1].padding)
                selections[count - 1].end = std::max(selections[count - 1].end, range.end);
            else selections[count++] = range;
        }
        selections.resize(count);
    }
    std::vector<std::string_view> lines;
    if (distribute && text.find_first_of("\r\n") != std::string_view::npos) {
        size_t begin = 0;
        for (size_t p = 0; p < text.size(); ++p) if (text[p] == '\r' || text[p] == '\n') {
            lines.push_back(text.substr(begin, p - begin));
            if (text[p] == '\r' && p + 1 < text.size() && text[p + 1] == '\n') ++p;
            begin = p + 1;
        }
        lines.push_back(text.substr(begin));
        if (lines.size() != selections.size()) lines.clear();
    }
    const size_t begin = selections.front().begin, end = selections.back().end;
    const size_t limit = maxFileBytes - (document.text.size() - (end - begin));
    std::string replacement;
    std::vector<ColumnRange> cursors;
    size_t offset = begin;
    for (size_t i = 0; i < selections.size(); ++i) {
        const auto& range = selections[i];
        const auto insert = lines.empty() ? text : lines[i];
        const size_t padding = insert.empty() ? 0 : static_cast<size_t>(range.padding);
        const size_t gap = range.begin - offset;
        if (gap > limit - replacement.size() || padding > limit - replacement.size() - gap || insert.size() > limit - replacement.size() - gap - padding) return false;
        replacement.append(document.text, offset, gap);
        replacement.append(padding, ' '); replacement += insert;
        const size_t caret = begin + replacement.size();
        const ColumnRange cursor{caret, caret, insert.empty() ? range.padding : 0, 0, insert.empty() && range.trailing};
        if (!cursors.empty() && cursors.back().begin == caret && cursors.back().padding == cursor.padding)
            cursors.back().trailing = cursors.back().trailing && cursor.trailing;
        else cursors.push_back(cursor);
        offset = range.end;
    }
    if (!buffer.replace(begin, end, std::move(replacement))) return false;
    ranges = std::move(cursors);
    return true;
}
}
