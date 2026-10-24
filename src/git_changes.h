#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace poppy {
inline constexpr size_t noGitChange = static_cast<size_t>(-1);
struct GitChangeNode {
    std::wstring path;
    size_t parent = noGitChange, entry = noGitChange;
    bool directory() const { return entry == noGitChange; }
};
// Paths are repository-relative and use '/'. Parents precede their children;
// file entries retain their input indices in both layouts. No disk I/O is used.
// Beyond the directory limit, remaining path components stay in file labels.
std::vector<GitChangeNode> buildGitChanges(const std::vector<std::wstring>& paths, bool tree, size_t maxDirectories = 50000);
}
