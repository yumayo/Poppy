#pragma once
#include <algorithm>

namespace poppy {
// Scroll padding is display space only; it never becomes a document row.
struct EditorScrollRange {
    int rows = 0, page = 1;
    bool padded = false;
    int pinned = 0;
    int totalRows() const {
        return std::max(1, rows) + (padded && rows ? std::max(0, page - pinned - 1) : 0);
    }
    int maximumTop() const { return std::max(0, totalRows() - page); }
    bool atTail(int top) const {
        // A newly opened short file still follows appends while fully visible.
        return top >= maximumTop() || (top == 0 && rows <= page);
    }
};
}
