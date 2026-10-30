#pragma once
#include "resource.h"
#include <functional>

namespace poppy {
enum class ExplorerEditKind { Move, Create, Delete };
// Creation and deletion move resources into private storage while absent.
// Restoration keeps the same contents, permissions and links.
struct ExplorerEdit {
    ResourceId before, after;
    ExplorerEditKind kind = ExplorerEditKind::Move;
    ResourceId selection = {};
    std::vector<ResourceId> expanded = {};
    const ResourceId& resource() const { return kind == ExplorerEditKind::Delete ? before : after; }
    bool restores(bool redo) const { return kind == ExplorerEditKind::Create ? redo : !redo; }
};
class ExplorerHistory {
public:
    using Rename = std::function<std::wstring(const ResourceId&, const ResourceId&)>;
    const ExplorerEdit* next(bool redo) const;
    ExplorerEdit* next(bool redo);
    std::wstring replay(bool redo, const Rename& rename);
    // Return abandoned backups for deletion by the provider, outside the UI.
    std::vector<ResourceId> record(ExplorerEdit edit);
    std::vector<ResourceId> clear();
private:
    std::vector<ExplorerEdit> edits_;
    size_t cursor_ = 0;
};
}
