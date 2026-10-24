#include "file_search.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("poppy-search-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { check(fs::create_directory(root), "Cannot create search fixture"); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    void file(const std::string& relative) {
        const auto path = root / fs::u8path(relative);
        fs::create_directories(path.parent_path()); std::ofstream stream(path); stream << "test\n";
        check(stream.good(), "Cannot create search fixture file");
    }
};
void matching() {
    check(fileSearchScore("src/App.cpp", "APP.CPP") == 0, "Exact filename or ASCII case matching failed");
    check(fileSearchScore("src/app.cpp", "ap") < fileSearchScore("src/wrapper.cpp", "ap"), "Filename prefix was not preferred");
    check(fileSearchScore("src/app.cpp", "app") < fileSearchScore("app/other.cpp", "app"), "Filename was not preferred over parent directory");
    check(fileSearchScore("src/quick_open.cpp", "qoc") >= 0, "Fuzzy filename matching failed");
    check(fileSearchScore("src/app.cpp", "src\\app") >= 0, "Relative paths or Windows separators failed");
    check(fileSearchScore(u8"資料/日本語.txt", u8"日本語") >= 0, "Japanese query did not match");
    check(fileSearchScore(u8"資料/日本語.txt", u8"語日") < 0, "Fuzzy matching ignored character order");
    check(fileSearchScore("src/app.cpp", "missing") < 0, "An unrelated file matched");
    check(fileSearchScore("src/app.cpp", "") == 0, "Empty query did not list files");
}
void traversal() {
    Fixture fixture;
    fixture.file("src/app.cpp"); fixture.file("app/other.cpp"); fixture.file("untracked.txt");
    fixture.file(u8"資料/日本語.txt"); fixture.file(".git/objects/hidden.txt"); fixture.file("nested/.git/hidden.txt");
    const auto index = indexFiles(fixture.root);
    check(index.error.empty() && index.files.size() == 4, "Startup file list did not collect every file");
    auto result = searchFiles(index, "");
    check(result.error.empty() && result.matches.size() == 4, "Traversal omitted files, included directories, or entered .git");
    result = searchFiles(index, "app");
    check(result.matches.size() == 2 && result.matches.front().relative.generic_u8string() == "src/app.cpp", "Search did not rank filename matches first");
    result = searchFiles(index, u8"日本");
    check(result.matches.size() == 1 && result.matches[0].relative.generic_u8string() == u8"資料/日本語.txt", "Traversal lost Unicode paths");
    std::error_code error;
    fs::create_directory_symlink(fixture.root, fixture.root / "loop", error);
    if (!error) check(indexFiles(fixture.root).files.size() == 4, "Traversal followed a directory symlink");
    check(!indexFiles(fixture.root / "missing").error.empty(), "An inaccessible root did not report an error");
}
void cachedFilteringAndRefresh() {
    Fixture fixture;
    fixture.file("old-file.txt"); fixture.file(u8"資料/日本語.txt");
    const auto initial = indexFiles(fixture.root);
    fixture.file("new-file.txt"); fs::remove(fixture.root / "old-file.txt");
    check(searchFiles(initial, "new-file").matches.empty(), "Typing performed an unexpected directory scan");
    check(searchFiles(initial, "old-file").matches.size() == 1, "Typing did not use the cached snapshot");
    const auto updated = indexFiles(fixture.root);
    check(searchFiles(updated, "new-file").matches.size() == 1 && searchFiles(updated, "old-file").matches.empty(), "Reindexing did not apply additions and deletions");
    fs::remove_all(fixture.root);
    check(searchFiles(initial, u8"日本").matches.size() == 1 && searchFiles(updated, "new-file").matches.size() == 1,
        "Filtering accessed the filesystem after the file list was collected");
}
void limitsAndCancellation() {
    Fixture fixture;
    for (const auto* name : {"z.txt", "a.txt", "m.txt", "b.txt"}) fixture.file(name);
    const auto index = indexFiles(fixture.root);
    const auto limited = searchFiles(index, "txt", {}, 2);
    check(limited.matches.size() == 2 && limited.resultsLimited, "Result limit was not reported");
    check(limited.matches[0].relative == "a.txt" && limited.matches[1].relative == "b.txt", "The bounded result list did not retain the best matches");
    size_t visited = 0;
    const auto cancelled = searchFiles(index, "txt", [&] { return ++visited == 3; });
    check(visited == 3 && cancelled.matches.empty(), "Cancelled searches retained stale results or continued scanning");
    visited = 0;
    check(indexFiles(fixture.root, [&] { return ++visited == 3; }).files.empty() && visited == 3,
        "Cancelled indexing retained a partial list or continued scanning");
    check(searchFiles(index, "", {}, 0).matches.empty(), "Zero result limit was ignored");
    const auto empty = searchFiles(index, "", {}, 2);
    check(empty.matches.size() == 2 && empty.resultsLimited && empty.matches[0].relative == "a.txt", "Empty query did not return the first cached files");
    FileIndex ranked;
    ranked.files = {{"a/needle-other.txt", U"a/needle-other.txt"}, {"z/needle.txt", U"z/needle.txt"}};
    const auto best = searchFiles(ranked, "needle.txt", {}, 1);
    check(best.matches.size() == 1 && best.matches[0].relative.generic_u8string() == "z/needle.txt" && best.resultsLimited,
        "A later exact match did not replace a weaker match in the bounded heap");
}
void completeFileList() {
    Fixture fixture;
    // Collect more than the displayed candidate limit, including deep folders.
    for (int i = 0; i < 125; ++i) fixture.file("file-" + std::to_string(i) + ".txt");
    std::string deep;
    for (int i = 0; i < 65; ++i) deep += "a/";
    fixture.file(deep + "deep.txt");
    auto index = indexFiles(fixture.root);
    check(index.files.size() == 126 && searchFiles(index, "deep.txt").matches.size() == 1,
        "Indexing stopped at the result limit or the old directory depth limit");
    // In-memory filtering must include files beyond the old 50,000-entry cap.
    index.files.assign(60000, {"a.txt", U"a.txt"});
    index.files.push_back({"z-last.txt", U"z-last.txt"});
    const auto last = searchFiles(index, "z-last");
    check(last.matches.size() == 1 && last.matches[0].relative == "z-last.txt", "Filtering stopped at 50,000 entries");
    check(searchFiles(index, "").matches.size() == 100, "The display limit should not limit the cached file list");
}
}
int main() {
    try { matching(); traversal(); cachedFilteringAndRefresh(); limitsAndCancellation(); completeFileList(); std::cout << "File search tests passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
