#include "file_search.h"
#include "text.h"
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace poppy {
namespace {
std::u32string normalized(std::string_view text) {
    std::u32string result;
    for (size_t p = 0; p < text.size();) {
        auto ch = decodeUtf8(text, p);
        if (ch == U'\\') ch = U'/';
        if (ch >= U'A' && ch <= U'Z') ch += U'a' - U'A';
        result += ch;
    }
    return result;
}
int matchScore(std::u32string_view name, std::u32string_view query) {
    const auto at = name.find(query);
    if (at != std::u32string_view::npos) {
        if (name == query) return 0;
        return (at == 0 ? 100 : 200) + static_cast<int>(std::min<size_t>(name.size() - query.size(), 99));
    }
    size_t position = 0, last = 0;
    for (size_t i = 0; i < query.size(); ++i) {
        position = name.find(query[i], position);
        if (position == std::u32string_view::npos) return -1;
        last = position++;
    }
    return 300 + static_cast<int>(std::min<size_t>(last + 1 - query.size(), 199));
}
int scorePath(std::u32string_view path, std::u32string_view query) {
    if (query.empty()) return 0;
    const auto slash = path.find_last_of(U'/');
    const auto filename = slash == std::u32string_view::npos ? path : path.substr(slash + 1);
    const int filenameScore = matchScore(filename, query);
    if (filenameScore >= 0) return filenameScore;
    const int pathScore = matchScore(path, query);
    return pathScore < 0 ? -1 : 1000 + pathScore;
}
}
int fileSearchScore(std::string_view relative, std::string_view query) {
    return scorePath(normalized(relative), normalized(query));
}
FileSearchResult searchFiles(const std::filesystem::path& root, std::string_view query,
    const std::function<bool()>& cancelled, size_t resultLimit, size_t scanLimit) {
    namespace fs = std::filesystem;
    FileSearchResult result;
    if (root.empty() || !resultLimit) return result;
    const auto needle = normalized(query);
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
    if (error) { result.error = error.message(); return result; }
    size_t visited = 0;
    const auto before = [](const FileMatch& a, const FileMatch& b) {
        if (a.score != b.score) return a.score < b.score;
        return a.relative.generic_u8string() < b.relative.generic_u8string();
    };
    for (; it != end; it.increment(error)) {
        if (cancelled && cancelled()) return {};
        if (error) { result.error = error.message(); break; }
        if (visited++ >= scanLimit) { result.scanLimited = true; break; }
        const auto& entry = *it;
        bool skip = entry.path().filename() == ".git" || entry.is_symlink(error);
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        skip = skip || attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#endif
        if (skip || it.depth() >= 64) {
            it.disable_recursion_pending();
            if (!skip) result.scanLimited = true;
            continue;
        }
        if (!entry.is_regular_file(error)) { error.clear(); continue; }
        auto relative = entry.path().lexically_relative(root);
        const int score = scorePath(normalized(relative.generic_u8string()), needle);
        if (score < 0) continue;
        FileMatch match{std::move(relative), score};
        if (result.matches.size() == resultLimit) {
            result.resultsLimited = true;
            if (!before(match, result.matches.back())) continue;
            result.matches.pop_back();
        }
        const auto position = std::lower_bound(result.matches.begin(), result.matches.end(), match, before);
        result.matches.insert(position, std::move(match));
    }
    if (error) result.error = error.message();
    return result;
}
}
