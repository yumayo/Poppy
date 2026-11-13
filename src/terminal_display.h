#pragma once
#include "composition.h"
#include "terminal_model.h"
#include <cstdint>
#include <utility>

namespace poppy {
// A temporary frame that shows composition text before the shell echoes it.
struct TerminalPrediction {
    std::vector<std::vector<Cell>> cells;
    CompositionLayout composition;
};
// Present parsed output immediately; retain the last complete frame only when
// the application explicitly requests synchronized output.
class TerminalDisplay {
public:
    void changed() { dirty_ = true; }
    bool update(TerminalModel& model, size_t scroll, uint64_t now, bool force = false);
    bool pending() const { return dirty_ || synchronizing_; }
    uint64_t synchronizationDeadline() const { return synchronizing_ ? syncSince_ + 1000 : 0; }
    int columns() const { return columns_; }
    int rows() const { return static_cast<int>(cells_.size()); }
    const std::vector<Cell>& row(int y) const { return cells_.at(static_cast<size_t>(y)); }
    int cursorX() const { return cursorX_; }
    int cursorY() const { return cursorY_; }
    bool cursorVisible() const { return cursorVisible_ && !scroll_; }
    std::string linkAt(int column, int row) const;
    // Locate composition text that the shell has echoed immediately before the
    // cursor. Glyphs use viewport cells; nothing is returned until every glyph
    // matches, so a pending or reformatted echo is never decorated.
    CompositionLayout echoedComposition(const CompositionText& text) const;
    // Draw composition text from an input position given in buffer rows.
    // Cells are overwritten, not shifted; cells the previous composition still
    // occupies up to the cursor are blanked. Empty when it does not fit.
    TerminalPrediction predictComposition(const CompositionText& text, int64_t row, int column) const;
    int64_t firstRow() const { return firstRow_; }
    // The input position after the cursor, including a pending autowrap.
    int inputColumn() const { return wrapPending_ ? columns_ : cursorX_; }
    // Selection endpoints are cell boundaries; the end is exclusive.
    std::pair<int, int> selectionRange(int anchor, int caret) const;
    std::string selectedText(int anchor, int caret) const;
    // Persistent selection uses buffer coordinates, independently of the viewport.
    void beginSelection(int point);
    void extendSelection(const TerminalModel& model, int point);
    void clearSelection() { anchor_ = caret_ = -1; selectionText_.clear(); }
    bool hasSelection() const { return anchor_ >= 0 && caret_ >= 0 && anchor_ != caret_; }
    std::pair<int, int> selectionRange() const;
    const std::string& selectedText() const { return selectionText_; }
private:
    std::vector<std::vector<Cell>> cells_;
    std::vector<int> wrapped_;
    int columns_ = 0, cursorX_ = 0, cursorY_ = 0;
    size_t scroll_ = 0;
    int64_t firstRow_ = 0, anchor_ = -1, caret_ = -1;
    uint64_t generation_ = 0;
    std::string selectionText_;
    bool cursorVisible_ = false, wrapPending_ = false, dirty_ = false, synchronizing_ = false;
    bool continuesAbove_ = false;
    uint64_t syncSince_ = 0;
};
}
