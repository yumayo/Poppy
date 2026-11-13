#include "csv_search.h"
#include <utility>

namespace poppy {
void CsvSearch::query(std::string text, bool matchCase) {
    pattern_.assign(std::move(text), matchCase);
    start({});
}
void CsvSearch::start(CsvPosition origin, bool backwards, bool skipOrigin) {
    cursor_ = {}; first_.reset(); last_.reset(); current_.reset();
    origin_ = origin; backwards_ = backwards; skipOrigin_ = skipOrigin; count_ = ordinal_ = 0;
    ready_ = pattern_.empty();
}
bool CsvSearch::matches(const CsvTable& table, std::string_view source, CsvPosition cell) const {
    if (pattern_.empty() || cell.row >= table.rows() || cell.column >= table.fields(cell.row)) return false;
    const auto value = table.value(source, cell);
    TextSearchCursor cursor;
    return pattern_.next(value, cursor, value.size()).has_value();
}
bool CsvSearch::step(const CsvTable& table, std::string_view source, size_t cellBudget) {
    if (ready_) return true;
    const auto before = [](CsvPosition a, CsvPosition b) { return a.row != b.row ? a.row < b.row : a.column < b.column; };
    for (size_t visited = 0; visited < cellBudget && cursor_.row < table.rows(); ++visited) {
        if (cursor_.column >= table.fields(cursor_.row)) { ++cursor_.row; cursor_.column = 0; continue; }
        const auto cell = cursor_; ++cursor_.column;
        if (!matches(table, source, cell)) continue;
        ++count_; last_ = cell;
        if (!first_) first_ = cell;
        const bool target = backwards_ ? before(cell, origin_) :
            !current_ && (skipOrigin_ ? before(origin_, cell) : !before(cell, origin_));
        if (target) { current_ = cell; ordinal_ = count_; }
    }
    if (cursor_.row >= table.rows()) {
        ready_ = true;
        if (!current_ && count_) {
            current_ = backwards_ ? last_ : first_;
            ordinal_ = backwards_ ? count_ : 1;
        }
    }
    return ready_;
}
}
