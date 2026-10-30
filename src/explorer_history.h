#pragma once
#include "resource.h"
#include <functional>

namespace poppy {
// Creation is represented by moving the new resource into private storage on
// undo. Redo restores the same object, including contents, permissions and links.
struct ExplorerEdit {
    ResourceId before, after;
    bool creation = false;
    ResourceId selection = {};
    std::vector<ResourceId> expanded = {};
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
