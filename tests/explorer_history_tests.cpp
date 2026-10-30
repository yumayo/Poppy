#include "explorer_history.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("poppy-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directory(root); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};
void write(const fs::path& path, const std::string& bytes) { std::ofstream out(path, std::ios::binary); out << bytes; check(bool(out), "Cannot write fixture"); }
std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
std::wstring renameResource(const ResourceId& from, const ResourceId& to) {
    if (fs::exists(to.local())) return L"Destination exists";
    std::error_code error; fs::rename(from.local(), to.local(), error);
    return error ? L"Rename failed" : L"";
}
void filesAndFolders() {
    Fixture fixture; const auto root = fixture.root / "project"; fs::create_directory(root);
    const auto original = root / fs::u8path("日本語 file.txt"), named = root / "renamed.txt", backup = fixture.root / ".poppy-undo-file";
    write(original, "created\r\n");
    ExplorerHistory history;
    check(!history.next(false) && !history.next(true), "New project had Explorer history");
    history.record({backup, original, true});
    check(renameResource(original, named).empty(), "Cannot rename fixture"); history.record({original, named, false});
    check(history.replay(false, renameResource).empty() && fs::exists(original) && !fs::exists(named), "Rename undo failed");
    write(original, "saved after creation");
    check(history.replay(false, renameResource).empty() && !fs::exists(original) && read(backup) == "saved after creation", "Creation undo did not remove/preserve the file");
    write(original, "external collision");
    check(!history.replay(true, renameResource).empty() && history.next(true)->creation && read(original) == "external collision", "Failed redo advanced history or overwrote a file");
    fs::remove(original);
    check(history.replay(true, renameResource).empty() && read(original) == "saved after creation" && !fs::exists(backup), "File redo lost saved contents");
    check(history.replay(true, renameResource).empty() && read(named) == "saved after creation", "Rename redo failed");
    const auto folder = root / "nested", moved = root / "destination", folderBackup = fixture.root / ".poppy-undo-folder";
    fs::create_directories(folder / "child"); write(folder / "child" / "data", "descendant");
    history.record({folderBackup, folder, true});
    check(renameResource(folder, moved).empty(), "Cannot move fixture"); history.record({folder, moved, false});
    check(history.replay(false, renameResource).empty() && read(folder / "child" / "data") == "descendant", "Folder move undo lost descendants");
    check(history.replay(false, renameResource).empty() && !fs::exists(folder) && read(folderBackup / "child" / "data") == "descendant", "Folder creation undo failed");
    check(history.replay(true, renameResource).empty() && read(folder / "child" / "data") == "descendant", "Folder creation redo failed");
    check(history.replay(true, renameResource).empty() && read(moved / "child" / "data") == "descendant", "Folder move redo failed");
    check(history.clear().empty() && fs::exists(moved) && fs::exists(named), "Clearing applied history deleted live resources");
}
void branchingAndFailures() {
    const auto root = ResourceId::wsl("Ubuntu", "/home/test/project");
    ExplorerHistory history;
    const auto first = root / std::string_view("a"), second = root / std::string_view("b"), backup = root.parent_path() / std::string_view(".poppy-undo-test");
    history.record({backup, first, true});
    check(!history.replay(false, [](const auto&, const auto&) { return L"Permission denied"; }).empty() && !history.next(true), "Failed undo advanced the cursor");
    check(history.replay(false, [&](const auto& from, const auto& to) { check(from == first && to == backup, "Undo crossed resources or authorities"); return L""; }).empty(), "Undo failed");
    auto discarded = history.record({first, second, false});
    check(discarded.size() == 1 && discarded.front() == backup && !history.next(true), "A new action did not discard the redo backup");
    history.clear();
    history.record({backup, first, true}); history.replay(false, [](const auto&, const auto&) { return L""; });
    discarded = history.clear(); check(discarded.size() == 1 && discarded.front() == backup, "Closing a project leaked its undone creation");
    for (int i = 0; i < 1001; ++i) history.record({first, second, false});
    int count = 0;
    while (history.next(false)) { check(history.replay(false, [](const auto&, const auto&) { return L""; }).empty(), "History limit broke undo"); ++count; }
    check(count == 1000, "Explorer history exceeded its bound");
}
}
int main() {
    try { filesAndFolders(); branchingAndFailures(); std::cout << "Explorer history tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
