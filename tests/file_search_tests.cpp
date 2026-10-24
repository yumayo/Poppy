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
    auto result = searchFiles(fixture.root, "");
    check(result.error.empty() && result.matches.size() == 4, "Traversal omitted files, included directories, or entered .git");
    result = searchFiles(fixture.root, "app");
    check(result.matches.size() == 2 && result.matches.front().relative.generic_u8string() == "src/app.cpp", "Search did not rank filename matches first");
    result = searchFiles(fixture.root, u8"日本");
    check(result.matches.size() == 1 && result.matches[0].relative.generic_u8string() == u8"資料/日本語.txt", "Traversal lost Unicode paths");
    fixture.file("new-file.txt");
    check(searchFiles(fixture.root, "new-file").matches.size() == 1, "A later search did not discover newly added files");
    fs::remove(fixture.root / "new-file.txt");
    check(searchFiles(fixture.root, "new-file").matches.empty(), "A later search retained deleted files");
    std::error_code error;
    fs::create_directory_symlink(fixture.root, fixture.root / "loop", error);
    if (!error) check(searchFiles(fixture.root, "").matches.size() == 4, "Traversal followed a directory symlink");
    check(!searchFiles(fixture.root / "missing", "").error.empty(), "An inaccessible root did not report an error");
}
void limitsAndCancellation() {
    Fixture fixture;
    for (const auto* name : {"z.txt", "a.txt", "m.txt", "b.txt"}) fixture.file(name);
    const auto limited = searchFiles(fixture.root, "txt", {}, 2);
    check(limited.matches.size() == 2 && limited.resultsLimited && !limited.scanLimited, "Result limit was not reported");
    check(limited.matches[0].relative == "a.txt" && limited.matches[1].relative == "b.txt", "The bounded result list did not retain the best matches");
    const auto scanned = searchFiles(fixture.root, "", {}, 100, 2);
    check(scanned.scanLimited && scanned.matches.size() == 2, "Traversal exceeded its scan limit");
    size_t visited = 0;
    const auto cancelled = searchFiles(fixture.root, "", [&] { return ++visited == 3; });
    check(visited == 3 && cancelled.matches.empty(), "Cancelled searches retained stale results or continued scanning");
    check(searchFiles(fixture.root, "", {}, 0).matches.empty(), "Zero result limit was ignored");
}
}
int main() {
    try { matching(); traversal(); limitsAndCancellation(); std::cout << "File search tests passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
