#pragma once
#include "text.h"

namespace poppy {
// A zero line number is an empty cell on that side of the comparison.
struct DiffLine { uint32_t row = 0, number = 0; };
struct DiffRow { DiffLine before, after; bool changed = false; };
struct DiffDocument {
    Document patch;
    std::vector<DiffRow> rows;
    uint32_t maxColumns = 0;
    std::string_view line(DiffLine source) const;
    size_t offset(DiffLine source) const;
    std::string selectedText(bool after, size_t start, size_t end) const;
};
void indexDiff(DiffDocument& document);
}
