#include "explorer_history.h"

namespace poppy {
ExplorerEdit* ExplorerHistory::next(bool redo) {
    return const_cast<ExplorerEdit*>(static_cast<const ExplorerHistory&>(*this).next(redo));
}
const ExplorerEdit* ExplorerHistory::next(bool redo) const {
    if (redo) return cursor_ < edits_.size() ? &edits_[cursor_] : nullptr;
    return cursor_ ? &edits_[cursor_ - 1] : nullptr;
}
std::wstring ExplorerHistory::replay(bool redo, const Rename& rename) {
    const auto edit = next(redo);
    if (!edit) return L"No Explorer operation to undo or redo.";
    const auto error = rename(redo ? edit->before : edit->after, redo ? edit->after : edit->before);
    if (error.empty()) { if (redo) ++cursor_; else --cursor_; }
    return error;
}
std::vector<ResourceId> ExplorerHistory::record(ExplorerEdit edit) {
    std::vector<ResourceId> abandoned;
    for (size_t i = cursor_; i < edits_.size(); ++i) if (edits_[i].kind == ExplorerEditKind::Create) abandoned.push_back(edits_[i].before);
    edits_.resize(cursor_); edits_.push_back(std::move(edit)); ++cursor_;
    constexpr size_t limit = 1000;
    if (edits_.size() > limit) {
        if (edits_.front().kind == ExplorerEditKind::Delete) abandoned.push_back(edits_.front().after);
        edits_.erase(edits_.begin()); --cursor_;
    }
    return abandoned;
}
std::vector<ResourceId> ExplorerHistory::clear() {
    std::vector<ResourceId> abandoned;
    for (size_t i = 0; i < edits_.size(); ++i) {
        if (i < cursor_ && edits_[i].kind == ExplorerEditKind::Delete) abandoned.push_back(edits_[i].after);
        if (i >= cursor_ && edits_[i].kind == ExplorerEditKind::Create) abandoned.push_back(edits_[i].before);
    }
    edits_.clear(); cursor_ = 0; return abandoned;
}
}
