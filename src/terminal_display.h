#pragma once
#include "terminal_model.h"
#include <cstdint>
#include <utility>

namespace poppy {
// Only the committed visible cells reach the renderer, including incidental
// paints caused by focus, exposure or resizing while output is incomplete.
class TerminalDisplay {
public:
    // Keep echo responsive briefly after typing, including delayed replies.
    void input(uint64_t now) { inputUntil_ = now + 250; }
    void changed(uint64_t now);
    void resized(uint64_t now);
    bool update(TerminalModel& model, size_t scroll, uint64_t now, bool queued = false, bool force = false);
    bool pending() const { return dirty_ || synchronizing_; }
    int columns() const { return columns_; }
    int rows() const { return static_cast<int>(cells_.size()); }
    const std::vector<Cell>& row(int y) const { return cells_.at(static_cast<size_t>(y)); }
    int cursorX() const { return cursorX_; }
    int cursorY() const { return cursorY_; }
    bool cursorVisible() const { return cursorVisible_ && !scroll_; }
    // Selection endpoints are cell boundaries; the end is exclusive.
    std::pair<int, int> selectionRange(int anchor, int caret) const;
    std::string selectedText(int anchor, int caret) const;
private:
    std::vector<std::vector<Cell>> cells_;
    int columns_ = 0, cursorX_ = 0, cursorY_ = 0;
    size_t scroll_ = 0;
    bool cursorVisible_ = false, dirty_ = false, synchronizing_ = false;
    uint64_t firstChange_ = 0, lastChange_ = 0, resizeUntil_ = 0, syncSince_ = 0, inputUntil_ = 0;
};
}
