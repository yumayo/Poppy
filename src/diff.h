#pragma once
#include "text.h"

namespace poppy {
// A zero line number is an empty cell on that side of the comparison.
struct DiffLine { uint32_t row = 0, number = 0; };
struct DiffRow { DiffLine before, after; bool changed = false; uint32_t heading = 0; };
struct DiffDocument {
    Document patch;
    std::vector<DiffRow> rows;
    // Split row index * 2, plus 1 for its added line. Context appears once.
    std::vector<uint32_t> inlineOrder;
    uint32_t maxColumns = 0;
    std::string_view line(DiffLine source) const;
    size_t offset(DiffLine source) const;
    size_t rowCount(bool inlineView) const { return inlineView ? inlineOrder.size() : rows.size(); }
    DiffRow displayRow(size_t index, bool inlineView) const;
    size_t rowForLine(DiffLine source, bool after, bool inlineView) const;
    size_t mapRow(size_t index, bool fromInline, bool toInline, bool after) const;
    std::string selectedText(bool after, size_t start, size_t end, bool inlineView = false) const;
};
void indexDiff(DiffDocument& document, bool fileHeadings = false);
}
