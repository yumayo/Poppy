#include "explorer_history.h"
#include <exception>

namespace poppy {
std::vector<ExplorerEdit>* ExplorerHistory::nextBatch(bool redo) {
    return const_cast<std::vector<ExplorerEdit>*>(static_cast<const ExplorerHistory&>(*this).nextBatch(redo));
}
const std::vector<ExplorerEdit>* ExplorerHistory::nextBatch(bool redo) const {
    if (redo) return cursor_ < edits_.size() ? &edits_[cursor_] : nullptr;
    return cursor_ ? &edits_[cursor_ - 1] : nullptr;
}
ExplorerEdit* ExplorerHistory::next(bool redo) {
    return const_cast<ExplorerEdit*>(static_cast<const ExplorerHistory&>(*this).next(redo));
}
const ExplorerEdit* ExplorerHistory::next(bool redo) const {
    const auto batch = nextBatch(redo);
    return batch ? &batch->front() : nullptr;
}
std::wstring ExplorerHistory::replay(bool redo, const Rename& rename, std::vector<ExplorerEdit>* applied) {
    if (applied) applied->clear();
    const auto batch = nextBatch(redo);
    if (!batch) return L"No Explorer operation to undo or redo.";
    size_t count = 0;
    std::wstring error;
    for (; count < batch->size(); ++count) {
        const auto& edit = (*batch)[redo ? count : batch->size() - 1 - count];
        try { error = rename(redo ? edit.before : edit.after, redo ? edit.after : edit.before); }
        catch (const std::exception& failure) { error = resourceWide(failure.what()); }
        if (!error.empty()) break;
        if (applied) applied->push_back(edit);
    }
    if (count == batch->size()) { if (redo) ++cursor_; else --cursor_; }
    else if (count) {
        const auto index = redo ? cursor_ : cursor_ - 1;
        const auto split = batch->begin() + (redo ? count : batch->size() - count);
        std::vector<ExplorerEdit> tail(split, batch->end());
        batch->erase(split, batch->end());
        edits_.insert(edits_.begin() + index + 1, std::move(tail));
        if (redo) ++cursor_;
    }
    return error;
}
std::vector<ResourceId> ExplorerHistory::record(ExplorerEdit edit) {
    std::vector<ExplorerEdit> batch; batch.push_back(std::move(edit));
    return recordBatch(std::move(batch));
}
std::vector<ResourceId> ExplorerHistory::recordBatch(std::vector<ExplorerEdit> edits) {
    std::vector<ResourceId> abandoned;
    if (edits.empty()) return abandoned;
    for (size_t i = cursor_; i < edits_.size(); ++i) for (const auto& edit : edits_[i])
        if (edit.kind == ExplorerEditKind::Create) abandoned.push_back(edit.before);
    edits_.resize(cursor_); edits_.push_back(std::move(edits)); ++cursor_;
    constexpr size_t limit = 1000;
    if (edits_.size() > limit) {
        for (const auto& edit : edits_.front()) if (edit.kind == ExplorerEditKind::Delete) abandoned.push_back(edit.after);
        edits_.erase(edits_.begin()); --cursor_;
    }
    return abandoned;
}
std::vector<ResourceId> ExplorerHistory::clear() {
    std::vector<ResourceId> abandoned;
    for (size_t i = 0; i < edits_.size(); ++i) for (const auto& edit : edits_[i]) {
        if (i < cursor_ && edit.kind == ExplorerEditKind::Delete) abandoned.push_back(edit.after);
        if (i >= cursor_ && edit.kind == ExplorerEditKind::Create) abandoned.push_back(edit.before);
    }
    edits_.clear(); cursor_ = 0; return abandoned;
}
}
