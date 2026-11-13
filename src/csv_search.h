#pragma once
#include "csv_table.h"
#include "text_search.h"

namespace poppy {
// Finds cells whose decoded value contains a literal query, in row-major
// order. Like TextSearch it keeps a count and a navigation target only, so a
// scan can be split into slices; restart it after any edit.
class CsvSearch {
public:
    void query(std::string text, bool matchCase = false);
    // Forward searches select the first match at or after origin (strictly
    // after when skipOrigin); backward searches the last one before origin.
    void start(CsvPosition origin, bool backwards = false, bool skipOrigin = false);
    bool step(const CsvTable& table, std::string_view source, size_t cellBudget);
    bool matches(const CsvTable& table, std::string_view source, CsvPosition cell) const;
    bool empty() const { return pattern_.empty(); }
    bool ready() const { return ready_; }
    size_t count() const { return count_; }
    size_t ordinal() const { return ordinal_; }
    const std::optional<CsvPosition>& current() const { return current_; }
private:
    LiteralPattern pattern_;
    CsvPosition cursor_, origin_;
    std::optional<CsvPosition> first_, last_, current_;
    size_t count_ = 0, ordinal_ = 0;
    bool backwards_ = false, skipOrigin_ = false, ready_ = true;
};
}
