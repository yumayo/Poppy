#pragma once
#include "diff.h"
#include "text_layout.h"

namespace poppy {
struct WrappedDiffRow { size_t row = 0, continuation = 0; };
class DiffLayout {
public:
    void build(const DiffDocument& document, bool inlineView, int columns);
    int columns() const { return columns_; }
    size_t rows() const { return starts_.empty() ? 0 : starts_.back(); }
    size_t bytes() const { return starts_.capacity() * sizeof(uint32_t); }
    WrappedDiffRow row(size_t visualRow) const;
    WrappedRow textRow(const DiffDocument& document, size_t visualRow, bool after) const;
    size_t rowForOffset(const DiffDocument& document, size_t logicalRow, size_t byte, bool after) const;
private:
    // Split replacements use the taller side, keeping subsequent rows aligned.
    std::vector<uint32_t> starts_;
    int columns_ = 0;
    bool inlineView_ = true;
};
}
