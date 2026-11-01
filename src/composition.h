#pragma once
#include "text.h"
#include <cstdint>

namespace poppy {
struct CompositionGlyph {
    char32_t character;
    int row, column, width;
    uint8_t attribute;
};
struct CompositionLayout {
    std::vector<CompositionGlyph> glyphs;
    int caretRow = 0, caretColumn = 0;
};
// IMM cursor positions and attributes are indexed by UTF-16 code units.
// Glyph positions use the same one/two-cell widths as the terminal and editor.
struct CompositionText {
    std::u16string text;
    std::vector<uint8_t> attributes;
    size_t cursor = 0;
    CompositionLayout layout(int columns, int firstColumn) const;
};
}
