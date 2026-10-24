#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
struct FileMatch { std::filesystem::path relative; int score; };
struct FileSearchResult {
    std::vector<FileMatch> matches;
    std::string error;
    bool scanLimited = false, resultsLimited = false;
};
// UTF-8 names and queries; slash direction and ASCII case are ignored.
// Lower scores prefer a matching filename over a matching parent directory.
int fileSearchScore(std::string_view relative, std::string_view query);
// On-demand traversal retains only the best results, without a permanent index.
FileSearchResult searchFiles(const std::filesystem::path& root, std::string_view query,
    const std::function<bool()>& cancelled = {}, size_t resultLimit = 100, size_t scanLimit = 50000);
}
