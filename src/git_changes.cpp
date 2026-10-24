#include "git_changes.h"
#include <algorithm>
#include <cwctype>
#include <map>
#include <string_view>

namespace poppy {
std::vector<GitChangeNode> buildGitChanges(const std::vector<std::wstring>& paths, bool tree, size_t maxDirectories) {
    std::vector<GitChangeNode> nodes;
    nodes.reserve(paths.size());
    std::map<std::wstring, size_t> directories;
    for (size_t i = 0; i < paths.size(); ++i) {
        size_t parent = noGitChange;
        if (tree) for (size_t slash = paths[i].find(L'/'); slash != std::wstring::npos; slash = paths[i].find(L'/', slash + 1)) {
            const auto path = paths[i].substr(0, slash);
            const auto found = directories.find(path);
            if (found != directories.end()) parent = found->second;
            else {
                if (directories.size() >= maxDirectories) break;
                const auto index = nodes.size();
                nodes.push_back({path, parent, noGitChange});
                directories.emplace(path, index); parent = index;
            }
        }
        nodes.push_back({paths[i], parent, i});
    }
    if (!tree) return nodes;

    std::vector<std::vector<size_t>> children(nodes.size() + 1);
    for (size_t i = 0; i < nodes.size(); ++i)
        children[nodes[i].parent == noGitChange ? nodes.size() : nodes[i].parent].push_back(i);
    for (auto& siblings : children) std::stable_sort(siblings.begin(), siblings.end(), [&](size_t a, size_t b) {
        if (nodes[a].directory() != nodes[b].directory()) return nodes[a].directory();
        const size_t offset = nodes[a].parent == noGitChange ? 0 : nodes[nodes[a].parent].path.size() + 1;
        const auto left = std::wstring_view(nodes[a].path).substr(offset), right = std::wstring_view(nodes[b].path).substr(offset);
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
            [](wchar_t x, wchar_t y) { return std::towlower(x) < std::towlower(y); });
    });
    std::vector<size_t> pending(children.back().rbegin(), children.back().rend()), indices(nodes.size());
    std::vector<GitChangeNode> sorted; sorted.reserve(nodes.size());
    while (!pending.empty()) {
        const auto index = pending.back(); pending.pop_back();
        auto node = std::move(nodes[index]);
        if (node.parent != noGitChange) node.parent = indices[node.parent];
        indices[index] = sorted.size(); sorted.push_back(std::move(node));
        pending.insert(pending.end(), children[index].rbegin(), children[index].rend());
    }
    return sorted;
}
}
