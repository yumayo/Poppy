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
bool addIndexedFile(FileIndex& index, std::string relative, size_t byteLimit, size_t fileLimit) {
    auto name = normalized(relative);
    // Account for string allocations and both vector buffers during growth.
    const size_t bytes = 3 * sizeof(IndexedFile) + relative.capacity() + 1 +
        (name.capacity() + 1) * sizeof(char32_t);
    if (index.files.size() >= fileLimit || index.storageBytes > byteLimit || bytes > byteLimit - index.storageBytes) {
        index.error = "File list reached its file-count or memory limit.";
        return false;
    }
    index.files.push_back({std::move(relative), std::move(name)});
    index.storageBytes += bytes;
    return true;
}
FileIndex indexFiles(const std::filesystem::path& root, const std::function<bool()>& cancelled) {
    namespace fs = std::filesystem;
    FileIndex result;
    if (root.empty()) return result;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
    if (error) { result.error = error.message(); return result; }
    for (; it != end; it.increment(error)) {
        if (cancelled && cancelled()) return {};
        if (error) { result.error = error.message(); break; }
        const auto& entry = *it;
        bool skip = entry.path().filename() == ".git" || entry.is_symlink(error);
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        skip = skip || attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#endif
        if (skip) {
            it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(error)) { error.clear(); continue; }
        auto relative = entry.path().lexically_relative(root).generic_u8string();
        auto name = normalized(relative);
        result.files.push_back({std::move(relative), std::move(name)});
    }
    if (error) result.error = error.message();
    if (cancelled && cancelled()) return {};
    std::sort(result.files.begin(), result.files.end(), [](const IndexedFile& a, const IndexedFile& b) { return a.relative < b.relative; });
    return result;
}
FileSearchResult searchFiles(const FileIndex& index, std::string_view query,
    const std::function<bool()>& cancelled, size_t resultLimit) {
    FileSearchResult result;
    result.error = index.error;
    if (!resultLimit) return result;
    const auto needle = normalized(query);
    if (needle.empty()) {
        const auto count = std::min(resultLimit, index.files.size());
        for (size_t i = 0; i < count; ++i) {
            if (cancelled && cancelled()) return {};
            result.matches.push_back({std::filesystem::u8path(index.files[i].relative), 0, index.files[i].relative});
        }
        result.resultsLimited = index.files.size() > count;
        return result;
    }
    struct Ranked { size_t index; int score; };
    const auto before = [](const Ranked& a, const Ranked& b) { return a.score != b.score ? a.score < b.score : a.index < b.index; };
    std::vector<Ranked> best;
    best.reserve(std::min(resultLimit, index.files.size()));
    for (size_t i = 0; i < index.files.size(); ++i) {
        if (cancelled && cancelled()) return {};
        const int score = scorePath(index.files[i].normalized, needle);
        if (score < 0) continue;
        const Ranked match{i, score};
        if (best.size() == resultLimit) {
            result.resultsLimited = true;
            if (!before(match, best.front())) continue;
            std::pop_heap(best.begin(), best.end(), before); best.back() = match;
        } else best.push_back(match);
        std::push_heap(best.begin(), best.end(), before);
    }
    std::sort_heap(best.begin(), best.end(), before);
    for (const auto& match : best) result.matches.push_back({std::filesystem::u8path(index.files[match.index].relative), match.score, index.files[match.index].relative});
    return result;
}
}
