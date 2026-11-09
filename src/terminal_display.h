#pragma once
#include "composition.h"
#include "terminal_model.h"
#include <cstdint>
#include <optional>
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
    // Keep composition at its insertion point while output moves the VT cursor.
    void beginPreedit();
    void endPreedit() { preeditCursor_.reset(); }
    // Reacquire the insertion point after the terminal echoes accepted input.
    void inputSent(std::u16string text);
    void cancelPreedit() { endPreedit(); pendingCommit_.reset(); }
    int inputCursorX() const { return preeditCursor_ ? preeditCursor_->x : softwareCursor_ ? softwareCursor_->x : cursorX_; }
    int inputCursorY() const { return preeditCursor_ ? preeditCursor_->y : softwareCursor_ ? softwareCursor_->y : cursorY_; }
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
    struct Cursor { int x, y; bool wrap; };
    std::optional<Cursor> preeditCursor_, softwareCursor_;
    std::optional<Cursor> findSoftwareCursor() const;
    bool inputWrapPending() const { return preeditCursor_ ? preeditCursor_->wrap : !softwareCursor_ && wrapPending_; }
    struct Commit { std::u16string text; int column; int64_t row; std::u32string prefix; };
    std::optional<Commit> pendingCommit_;
    bool commitPresented() const;
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
