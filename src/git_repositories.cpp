#include "git_repositories.h"
#include <algorithm>
#include <deque>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace poppy {
GitRepositoryScan scanGitRepositories(const std::filesystem::path& root,
    const std::function<bool()>& cancelled,
    const std::function<void(const std::filesystem::path&)>& visited,
    size_t directoryLimit, size_t entryLimit) {
    namespace fs = std::filesystem;
    GitRepositoryScan result;
    if (root.empty()) return result;
    std::deque<fs::path> pending{root};
    size_t directories = 1, entries = 0;
    while (!pending.empty()) {
        if (cancelled && cancelled()) return {};
        auto directory = std::move(pending.front()); pending.pop_front();
        if (visited) visited(directory);
        std::error_code error;
        fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
        if (error) { if (result.error.empty()) result.error = error.message(); continue; }
        for (; it != end; it.increment(error)) {
            if (cancelled && cancelled()) return {};
            if (error) { if (result.error.empty()) result.error = error.message(); break; }
            if (++entries > entryLimit) { result.truncated = true; pending.clear(); break; }
            const auto& entry = *it;
            if (entry.is_symlink(error)) continue;
#ifdef _WIN32
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
#endif
            if (entry.path().filename() == ".git") {
                if ((entry.is_directory(error) || entry.is_regular_file(error)) && validUtf8(directory.u8string())) result.roots.emplace_back(directory);
                continue;
            }
            if (!entry.is_directory(error)) { error.clear(); continue; }
            if (directories >= directoryLimit) { result.truncated = true; continue; }
            ++directories; pending.push_back(entry.path());
        }
    }
    std::sort(result.roots.begin(), result.roots.end(), [](const ResourceId& a, const ResourceId& b) { return a.u8string() < b.u8string(); });
    return result;
}
ResourceId gitRepositoryFor(const std::vector<ResourceId>& roots, const ResourceId& resource) {
    ResourceId result;
    for (const auto& root : roots) if (root.contains(resource) && (result.empty() || result.contains(root))) result = root;
    return result;
}
}
