#pragma once
#include "resource.h"
#include <functional>

namespace poppy {
struct GitRepositoryScan {
    std::vector<ResourceId> roots;
    std::string error;
    bool truncated = false;
};
// Find worktrees by their .git directory or gitfile. Git validates candidates
// later. Never enter .git, directory links or Windows reparse points.
GitRepositoryScan scanGitRepositories(const std::filesystem::path& root,
    const std::function<bool()>& cancelled = {},
    const std::function<void(const std::filesystem::path&)>& visited = {},
    size_t directoryLimit = 50000, size_t entryLimit = 200000);
// The innermost worktree owns a resource, including identical sibling filenames.
ResourceId gitRepositoryFor(const std::vector<ResourceId>& roots, const ResourceId& resource);
}
