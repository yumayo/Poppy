#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
constexpr size_t maxIndexedFiles = 200000, maxIndexBytes = 64 * 1024 * 1024, maxIndexDirectories = 50000;
struct FileMatch { std::filesystem::path relative; int score; std::string exactRelative; };
struct IndexedFile { std::string relative; std::u32string normalized; };
struct FileIndex {
    std::vector<IndexedFile> files;
    std::string error;
    size_t storageBytes = 0;
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
// Bound retained remote entries, including UTF-32 names and vector growth.
// A full index keeps its existing files and reports an error without throwing.
bool addIndexedFile(FileIndex& index, std::string relative,
    size_t byteLimit = maxIndexBytes, size_t fileLimit = maxIndexedFiles);
// Filter an immutable snapshot without filesystem access. Paths are normalized
// while indexing; queries only retain the best results in a bounded heap.
FileSearchResult searchFiles(const FileIndex& index, std::string_view query,
    const std::function<bool()>& cancelled = {}, size_t resultLimit = 100);
}
