#include "csv_diff.h"
#include <algorithm>

namespace poppy {
namespace {
// Rebuilds one side's text and the start of each of its source lines.
void rebuild(const DiffDocument& diff, bool after, CsvComparison::Side& side, std::vector<size_t>& starts) {
    for (const auto& row : diff.rows) {
        const auto& line = after ? row.after : row.before;
        if (!line.number) continue;
        if (!starts.empty()) side.text += '\n';
        starts.push_back(side.text.size());
        side.text.append(diff.line(line));
    }
    side.empty = starts.empty() || (starts.size() == 1 && side.text.empty());
}
}
std::string CsvComparison::value(bool afterSide, size_t row, size_t column) const {
    const auto& side = afterSide ? after : before;
    const auto record = afterSide ? rows[row].after : rows[row].before;
    return record == none ? std::string{} : side.table.value(side.text, {record, column});
}
bool CsvComparison::cellChanged(size_t row, size_t column) const {
    const auto& r = rows[row];
    if (r.before == none || r.after == none) return true;
    const bool old = column < before.table.fields(r.before), current = column < after.table.fields(r.after);
    if (old != current) return true;
    return old && before.table.value(before.text, {r.before, column}) != after.table.value(after.text, {r.after, column});
}
size_t CsvComparison::bytes() const {
    return before.text.capacity() + after.text.capacity() + before.table.bytes() + after.table.bytes() + rows.capacity() * sizeof(Row);
}
CsvComparison compareCsv(const DiffDocument& diff) {
    CsvComparison result;
    std::vector<size_t> beforeLines, afterLines;
    rebuild(diff, false, result.before, beforeLines);
    rebuild(diff, true, result.after, afterLines);
    for (auto* side : {&result.before, &result.after}) {
        if (side->empty || side->table.parse(side->text)) continue;
        result.error = (side == &result.before ? "Before: " : "After: ") + side->table.error();
        return result;
    }
    const size_t beforeRows = result.before.rows(), afterRows = result.after.rows();
    if (beforeRows > CsvTable::maxCells - std::min(CsvTable::maxCells, afterRows)) { result.error = "CSV comparison exceeds the table size limit"; return result; }
    result.columns = std::max(result.before.empty ? 0 : result.before.table.columns(), result.after.empty ? 0 : result.after.table.columns());
    const auto record = [](const CsvComparison::Side& side, const std::vector<size_t>& starts, uint32_t number) {
        // The working comparison's empty EOF line starts no record.
        const auto offset = starts[number - 1];
        if (side.empty || offset >= side.text.size()) return CsvComparison::none;
        return side.table.position(offset).row;
    };
    size_t old = 0, current = 0;
    const auto pairUntil = [&](size_t oldEnd, size_t newEnd) {
        while (old < oldEnd || current < newEnd) {
            CsvComparison::Row row;
            if (old < oldEnd) row.before = old++;
            if (current < newEnd) row.after = current++;
            result.rows.push_back(row);
        }
    };
    for (const auto& row : diff.rows) {
        if (row.changed || !row.before.number || !row.after.number) continue;
        const auto b = record(result.before, beforeLines, row.before.number), a = record(result.after, afterLines, row.after.number);
        // Every line of a multi-line record links the same pair; later links
        // that cross an earlier pair are ignored.
        if (b == CsvComparison::none || a == CsvComparison::none || b < old || a < current) continue;
        pairUntil(b, a);
        result.rows.push_back({old++, current++});
    }
    pairUntil(beforeRows, afterRows);
    for (size_t i = 0; i < result.rows.size(); ++i) {
        auto& row = result.rows[i];
        if (row.before == CsvComparison::none || row.after == CsvComparison::none) { row.changed = true; continue; }
        const size_t columns = std::max(result.before.table.fields(row.before), result.after.table.fields(row.after));
        for (size_t c = 0; c < columns && !row.changed; ++c) row.changed = result.cellChanged(i, c);
    }
    return result;
}
}
