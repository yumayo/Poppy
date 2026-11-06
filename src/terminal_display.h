#pragma once
#include "composition.h"
#include "terminal_model.h"
#include <cstdint>
#include <utility>

namespace poppy {
// A temporary view only: neither the terminal buffer nor its selection includes
// preedit text. Layout coordinates refer to cells in the displayed viewport.
struct TerminalPreedit {
    std::vector<std::vector<Cell>> cells;
    CompositionLayout composition;
    Color compositionBackground = background;
    int left = 0, right = 0, firstRow = 0, lastRow = 0;
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
    TerminalPreedit preedit(const CompositionText& text) const;
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
