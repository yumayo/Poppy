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
    scroll = std::min(scroll, model.historySize());
    const bool scrollChanged = scroll != scroll_;
    const bool completedSync = synchronizing_ && !model.synchronizedOutput();
    if (model.synchronizedOutput()) {
        if (!synchronizing_) { synchronizing_ = true; syncSince_ = now; }
        // A missing end marker must not leave the display frozen indefinitely.
        if (!force && now - syncSince_ < 1000) return false;
        model.endSynchronizedOutput();
        force = true;
    }
    synchronizing_ = false;
    if (!dirty_ && !force && !scrollChanged) return false;
    // Echo uses a frame deadline from the first chunk: a quiet-period debounce
    // keeps restarting during key repeat. Still let queued/resize bursts settle.
    const bool waitForEcho = now < inputUntil_ ? now - firstChange_ < 16 : now - lastChange_ < 32;
    // User scrolling bypasses batching, while the sync guard above still holds.
    if (!force && !completedSync && !scrollChanged && now - firstChange_ < 120 &&
        (queued || now < resizeUntil_ || waitForEcho)) return false;

    columns_ = model.columns(); scroll_ = scroll;
    cells_.resize(static_cast<size_t>(model.rows()));
    for (int y = 0; y < model.rows(); ++y) {
        cells_[y] = model.row(y, scroll_); cells_[y].resize(static_cast<size_t>(columns_));
    }
    cursorX_ = model.cursorX(); cursorY_ = model.cursorY(); cursorVisible_ = model.cursorVisible();
    dirty_ = false; resizeUntil_ = 0;
    return true;
}
std::pair<int, int> TerminalDisplay::selectionRange(int anchor, int caret) const {
    if (anchor < 0 || caret < 0 || !columns_) return {0, 0};
    const int limit = columns_ * rows();
    int begin = std::clamp(std::min(anchor, caret), 0, limit);
    int end = std::clamp(std::max(anchor, caret), 0, limit);
    if (begin == end) return {begin, end};
    // Include a whole wide character when either boundary cuts its cells.
    if (row(begin / columns_)[begin % columns_].continuation) --begin;
    if (end < limit && row(end / columns_)[end % columns_].continuation) ++end;
    return {begin, end};
}
std::string TerminalDisplay::selectedText(int anchor, int caret) const {
    const auto [begin, end] = selectionRange(anchor, caret);
    std::string result;
    for (int position = begin; position < end;) {
        const int y = position / columns_, rowEnd = std::min(end, (y + 1) * columns_);
        std::string part;
        for (; position < rowEnd; ++position) {
            const auto& cell = row(y)[position % columns_];
            if (!cell.continuation) appendUtf8(part, cell.ch);
        }
        while (!part.empty() && part.back() == ' ') part.pop_back();
        result += part;
        if (position < end) result += "\r\n";
    }
    return result;
}
}
