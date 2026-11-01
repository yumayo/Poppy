#include "composition.h"
#include <algorithm>

namespace poppy {
CompositionLayout CompositionText::layout(int columns, int firstColumn) const {
    columns = std::max(2, columns);
    int row = std::max(0, firstColumn) / columns, column = std::max(0, firstColumn) % columns;
    const auto caret = std::min(cursor, text.size());
    CompositionLayout result;
    for (size_t offset = 0; offset < text.size();) {
        const auto begin = offset;
        char32_t character = text[offset++];
        if (character >= 0xd800 && character <= 0xdbff && offset < text.size() && text[offset] >= 0xdc00 && text[offset] <= 0xdfff)
            character = 0x10000 + ((character - 0xd800) << 10) + (text[offset++] - 0xdc00);
        else if (character >= 0xd800 && character <= 0xdfff) character = 0xfffd;
        if (character < 32) character = U' ';
        const int width = std::max(1, cellWidth(character));
        if (column + width > columns) { ++row; column = 0; }
        if (caret >= begin && caret < offset) { result.caretRow = row; result.caretColumn = column; }
        result.glyphs.push_back({character, row, column, width, begin < attributes.size() ? attributes[begin] : uint8_t{0}});
        column += width;
        if (column == columns) { ++row; column = 0; }
    }
    if (caret == text.size()) { result.caretRow = row; result.caretColumn = column; }
    return result;
}
}
