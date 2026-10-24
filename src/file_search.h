#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
struct FileMatch { std::filesystem::path relative; int score; };
struct IndexedFile { std::string relative; std::u32string normalized; };
struct FileIndex {
    std::vector<IndexedFile> files;
    std::string error;
};
struct FileSearchResult {
    std::vector<FileMatch> matches;
    std::string error;
    bool resultsLimited = false;
};
// UTF-8 names and queries; slash direction and ASCII case are ignored.
// Lower scores prefer a matching filename over a matching parent directory.
int fileSearchScore(std::string_view relative, std::string_view query);
// Collect every regular file once, excluding .git and links/junctions.
FileIndex indexFiles(const std::filesystem::path& root, const std::function<bool()>& cancelled = {});
// Filter an immutable snapshot without filesystem access. Paths are normalized
// while indexing; queries only retain the best results in a bounded heap.
FileSearchResult searchFiles(const FileIndex& index, std::string_view query,
    const std::function<bool()>& cancelled = {}, size_t resultLimit = 100);
}
