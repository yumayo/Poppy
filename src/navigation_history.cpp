#include "navigation_history.h"

namespace poppy {
void NavigationHistory::visit(NavigationTarget target) {
    if (navigating_ || !target.project || (!target.editor && target.terminal.empty())) return;
    if (!entries_.empty()) {
        if (entries_[current_] == target) { entries_[current_] = std::move(target); return; }
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(current_ + 1), entries_.end());
    }
    entries_.push_back(std::move(target));
    if (entries_.size() > limit) entries_.erase(entries_.begin());
    current_ = entries_.size() - 1;
}
bool NavigationHistory::move(bool forward, const std::function<bool(const NavigationTarget&)>& activate) {
    if (navigating_ || entries_.empty()) return false;
    struct Guard {
        bool& flag;
        explicit Guard(bool& value) : flag(value) { flag = true; }
        ~Guard() { flag = false; }
    } guard(navigating_);
    size_t candidate = current_;
    while (forward ? candidate + 1 < entries_.size() : candidate > 0) {
        if (forward) ++candidate; else --candidate;
        if (entries_[candidate] == entries_[current_]) continue;
        if (activate(entries_[candidate])) { current_ = candidate; return true; }
    }
    return false;
}
}
