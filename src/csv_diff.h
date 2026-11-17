#pragma once
#include "csv_table.h"
#include "diff.h"
#include <limits>

namespace poppy {
// Two CSV tables rebuilt from a single file's complete diff, with their
// records aligned side by side. Unchanged diff lines pair the records they
// belong to; records between those pairs are paired in order, and the rest
// are removals or additions. Cells compare by column index.
struct CsvComparison {
    static constexpr size_t none = std::numeric_limits<size_t>::max();
    struct Side { std::string text; CsvTable table; bool empty = true; size_t rows() const { return empty ? 0 : table.rows(); } };
    struct Row { size_t before = none, after = none; bool changed = false; };
    Side before, after;
    std::vector<Row> rows;
    size_t columns = 0;
    std::string error;
    bool valid() const { return error.empty(); }
    std::string value(bool after, size_t row, size_t column) const;
    // A missing field differs from an empty one only when the other side has it.
    bool cellChanged(size_t row, size_t column) const;
    size_t bytes() const;
};
// The diff must hold one file's complete patch, such as a Git diff with
// unlimited context or a working comparison.
CsvComparison compareCsv(const DiffDocument& diff);
}
