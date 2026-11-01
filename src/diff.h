#pragma once
#include "text.h"
#include <optional>

namespace poppy {
// A zero line number is an empty cell on that side of the comparison.
struct DiffLine {
    uint32_t row = 0, number = 0;
    Language language = Language::Plain;
    // Lexer state at the start of this source line, independently for each side.
    LexState state = LexState::Normal;
};
struct DiffRow { DiffLine before, after; bool changed = false; uint32_t heading = 0; };
struct DiffFile { size_t row = 0; std::string path; };
struct DiffDocument {
    Document patch;
    std::vector<DiffRow> rows;
    // File boundaries in split-row order, separate from other metadata headings.
    std::vector<DiffFile> files;
    // Split row index * 2, plus 1 for its added line. Context appears once.
    std::vector<uint32_t> inlineOrder;
    uint32_t maxColumns = 0;
    // Editable comparisons retain every working line, including the EOF caret
    // row. These maps translate patch selections to real file byte offsets.
    struct WorkingLine { size_t patch, file, length; };
    std::vector<WorkingLine> workingLines;
    std::optional<size_t> workingOffset(size_t patchOffset) const;
    size_t patchOffset(size_t fileOffset) const;
    std::string_view line(DiffLine source) const;
    size_t offset(DiffLine source) const;
    size_t rowCount(bool inlineView) const { return inlineView ? inlineOrder.size() : rows.size(); }
    DiffRow displayRow(size_t index, bool inlineView) const;
    const DiffFile* fileForRow(size_t index, bool inlineView) const;
    size_t rowForLine(DiffLine source, bool after, bool inlineView) const;
    // Maps a visible text range in the patch to an inline row, excluding metadata and +/- prefixes.
    std::optional<size_t> inlineRowForText(size_t begin, size_t end) const;
    size_t mapRow(size_t index, bool fromInline, bool toInline, bool after) const;
    std::string selectedText(bool after, size_t start, size_t end, bool inlineView = false) const;
};
void indexDiff(DiffDocument& document, bool fileHeadings = false);
// Reorder complete file patches in a document indexed with file headings.
// Paths absent from the requested order remain at the end in their input order.
void orderDiffFiles(DiffDocument& document, const std::vector<std::string>& paths);
DiffDocument workingDiff(const Document& before, const Document& after, const std::string& path);
// One flag per working line, using the same alignment as the editable diff.
// Deleted lines have no working row; unchanged EOF caret rows stay unmarked.
std::vector<bool> changedWorkingLines(const Document& before, const Document& after);
}
