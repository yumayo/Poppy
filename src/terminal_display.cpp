#include "terminal_display.h"
#include <algorithm>

namespace poppy {
void TerminalDisplay::changed(uint64_t now) {
    if (!dirty_) firstChange_ = now;
    dirty_ = true; lastChange_ = now;
}
void TerminalDisplay::resized(uint64_t now) {
    changed(now); resizeUntil_ = now + 100;
}
bool TerminalDisplay::update(TerminalModel& model, size_t scroll, uint64_t now, bool queued, bool force) {
    const bool completedSync = synchronizing_ && !model.synchronizedOutput();
    if (model.synchronizedOutput()) {
        if (!synchronizing_) { synchronizing_ = true; syncSince_ = now; }
        // A missing end marker must not leave the display frozen indefinitely.
        if (!force && now - syncSince_ < 1000) return false;
        model.endSynchronizedOutput();
        force = true;
    }
    synchronizing_ = false;
    if (!dirty_ && !force) return false;
    // Let bursts settle, but continue to show progress during unbounded output.
    if (!force && !completedSync && now - firstChange_ < 120 &&
        (queued || now < resizeUntil_ || now - lastChange_ < 32)) return false;

    columns_ = model.columns(); scroll_ = std::min(scroll, model.historySize());
    cells_.resize(static_cast<size_t>(model.rows()));
    for (int y = 0; y < model.rows(); ++y) {
        cells_[y] = model.row(y, scroll_); cells_[y].resize(static_cast<size_t>(columns_));
    }
    cursorX_ = model.cursorX(); cursorY_ = model.cursorY(); cursorVisible_ = model.cursorVisible();
    dirty_ = false; resizeUntil_ = 0;
    return true;
}
}
