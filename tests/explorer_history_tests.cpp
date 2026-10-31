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
    history.record({backup, original, ExplorerEditKind::Create});
    check(renameResource(original, named).empty(), "Cannot rename fixture"); history.record({original, named, ExplorerEditKind::Move});
    check(history.replay(false, renameResource).empty() && fs::exists(original) && !fs::exists(named), "Rename undo failed");
    write(original, "saved after creation");
    check(history.replay(false, renameResource).empty() && !fs::exists(original) && read(backup) == "saved after creation", "Creation undo did not remove/preserve the file");
    write(original, "external collision");
    check(!history.replay(true, renameResource).empty() && history.next(true)->kind == ExplorerEditKind::Create && read(original) == "external collision", "Failed redo advanced history or overwrote a file");
    fs::remove(original);
    check(history.replay(true, renameResource).empty() && read(original) == "saved after creation" && !fs::exists(backup), "File redo lost saved contents");
    check(history.replay(true, renameResource).empty() && read(named) == "saved after creation", "Rename redo failed");
    const auto folder = root / "nested", moved = root / "destination", folderBackup = fixture.root / ".poppy-undo-folder";
    fs::create_directories(folder / "child"); write(folder / "child" / "data", "descendant");
    history.record({folderBackup, folder, ExplorerEditKind::Create});
    check(renameResource(folder, moved).empty(), "Cannot move fixture"); history.record({folder, moved, ExplorerEditKind::Move});
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
    history.record({backup, first, ExplorerEditKind::Create});
    check(!history.replay(false, [](const auto&, const auto&) { return L"Permission denied"; }).empty() && !history.next(true), "Failed undo advanced the cursor");
    check(history.replay(false, [&](const auto& from, const auto& to) { check(from == first && to == backup, "Undo crossed resources or authorities"); return L""; }).empty(), "Undo failed");
    auto discarded = history.record({first, second, ExplorerEditKind::Move});
    check(discarded.size() == 1 && discarded.front() == backup && !history.next(true), "A new action did not discard the redo backup");
    history.clear();
    history.record({backup, first, ExplorerEditKind::Create}); history.replay(false, [](const auto&, const auto&) { return L""; });
    discarded = history.clear(); check(discarded.size() == 1 && discarded.front() == backup, "Closing a project leaked its undone creation");
    for (int i = 0; i < 1001; ++i) history.record({first, second, ExplorerEditKind::Move});
    int count = 0;
    while (history.next(false)) { check(history.replay(false, [](const auto&, const auto&) { return L""; }).empty(), "History limit broke undo"); ++count; }
    check(count == 1000, "Explorer history exceeded its bound");
}
void deletions() {
    Fixture fixture; const auto root = fixture.root / "project"; fs::create_directory(root);
    const auto file = root / fs::u8path("日本語 file.txt"), backup = fixture.root / ".poppy-undo-deleted";
    write(file, "saved contents");
    ExplorerHistory history;
    check(renameResource(file, backup).empty(), "Cannot delete fixture file");
    history.record({file, backup, ExplorerEditKind::Delete, file});
    check(!fs::exists(file) && history.next(false)->resource() == ResourceId(file) &&
        history.next(false)->restores(false) && !history.next(false)->restores(true), "Deletion lost its logical path or replay direction");
    write(file, "external collision");
    check(!history.replay(false, renameResource).empty() && !history.next(true) && read(file) == "external collision" &&
        read(backup) == "saved contents", "Deletion undo overwrote a collision or advanced history");
    fs::remove(file);
    check(history.replay(false, renameResource).empty() && read(file) == "saved contents" && !fs::exists(backup), "File deletion undo lost contents");
    write(file, "saved after restore");
    check(history.replay(true, renameResource).empty() && !fs::exists(file) && read(backup) == "saved after restore", "File deletion redo lost later edits");
    check(history.replay(false, renameResource).empty(), "Cannot restore deleted file again");
    check(history.record({file, root / "other", ExplorerEditKind::Move}).empty() && fs::exists(file), "Discarding deletion redo removed a live resource");
    check(history.clear().empty(), "Clearing an undone deletion removed a live resource");

    const auto folder = root / "folder", folderBackup = fixture.root / ".poppy-undo-directory";
    fs::create_directories(folder / "nested"); write(folder / "nested" / "child", "descendant");
    check(renameResource(folder, folderBackup).empty(), "Cannot delete fixture folder");
    history.record({folder, folderBackup, ExplorerEditKind::Delete, folder});
    check(history.replay(false, renameResource).empty() && read(folder / "nested" / "child") == "descendant", "Folder deletion undo lost descendants");
    check(history.replay(true, renameResource).empty() && !fs::exists(folder), "Folder deletion redo failed");
    const auto abandoned = history.clear();
    check(abandoned.size() == 1 && abandoned.front() == ResourceId(folderBackup), "Closing history leaked a deleted folder backup");
    for (const auto& path : abandoned) fs::remove_all(path.local());
    check(fs::exists(file) && !fs::exists(folderBackup), "Backup cleanup affected a restored file");
}
void deletionCleanup() {
    const auto root = ResourceId::wsl("Ubuntu", "/home/test/project");
    const auto file = root / std::string_view("file"), backup = root.parent_path() / std::string_view(".poppy-undo-deleted");
    ExplorerHistory history;
    history.record({file, backup, ExplorerEditKind::Delete});
    for (int i = 0; i < 999; ++i) check(history.record({file, file, ExplorerEditKind::Move}).empty(), "Deletion backup was removed while still undoable");
    auto abandoned = history.record({file, file, ExplorerEditKind::Move});
    check(abandoned.size() == 1 && abandoned.front() == backup, "History eviction leaked a deleted resource");
    history.clear();
    history.record({file, backup, ExplorerEditKind::Delete});
    const auto newFile = root / std::string_view("created"), newBackup = root.parent_path() / std::string_view(".poppy-undo-created");
    history.record({newBackup, newFile, ExplorerEditKind::Create});
    check(history.replay(false, [](const auto&, const auto&) { return L""; }).empty(), "Cannot undo mixed history");
    abandoned = history.clear();
    check(abandoned.size() == 2 && abandoned[0] == backup && abandoned[1] == newBackup, "Mixed history cleanup missed applied deletions or undone creations");
}
void batches() {
    Fixture fixture;
    const ResourceId a = fixture.root / "a", b = fixture.root / "b";
    const ResourceId backupA = fixture.root / "backup-a", backupB = fixture.root / "backup-b";
    write(a.local(), "first"); write(b.local(), "second");
    check(renameResource(a, backupA).empty() && renameResource(b, backupB).empty(), "Cannot prepare batch deletion");
    ExplorerHistory history;
    history.recordBatch({{a, backupA, ExplorerEditKind::Delete, a}, {b, backupB, ExplorerEditKind::Delete, b}});
    std::vector<ExplorerEdit> applied;
    check(history.replay(false, renameResource, &applied).empty() && applied.size() == 2 &&
        read(a.local()) == "first" && read(b.local()) == "second" && !history.next(false), "One undo did not restore the whole batch");
    check(history.replay(true, renameResource, &applied).empty() && applied.size() == 2 && !fs::exists(a.local()) && !fs::exists(b.local()), "One redo did not repeat the whole batch");
    write(a.local(), "collision");
    check(!history.replay(false, renameResource, &applied).empty() && applied.size() == 1 &&
        read(a.local()) == "collision" && read(b.local()) == "second" && history.nextBatch(false)->size() == 1 &&
        history.nextBatch(true)->size() == 1, "A partially failed undo lost completed work, the collision or retry history");
    fs::remove(a.local());
    check(history.replay(false, renameResource).empty() && read(a.local()) == "first", "A partial batch could not be retried");
    check(history.replay(true, renameResource).empty() && history.replay(true, renameResource).empty(), "Partial undo history could not be redone");
    const auto abandoned = history.clear();
    check(abandoned.size() == 2 && fs::exists(backupA.local()) && fs::exists(backupB.local()), "Batch cleanup missed a deletion backup");

    history.recordBatch({{backupA, a, ExplorerEditKind::Create}, {backupB, b, ExplorerEditKind::Create}});
    history.replay(false, [](const auto&, const auto&) { return L""; });
    const auto discarded = history.record({a, b, ExplorerEditKind::Move});
    check(discarded.size() == 2, "Branching history leaked batch creation backups");
    history.clear();
    history.recordBatch({{backupA, a, ExplorerEditKind::Create}, {backupB, b, ExplorerEditKind::Create}});
    history.replay(false, [](const auto&, const auto&) { return L""; });
    int count = 0;
    check(!history.replay(true, [&](const auto&, const auto&) -> std::wstring { if (++count == 2) throw std::runtime_error("Disconnected"); return {}; }, &applied).empty() &&
        applied.size() == 1 && history.nextBatch(false)->size() == 1 && history.nextBatch(true)->size() == 1, "A partially failed redo lost completed work or advanced past a failed edit");
}
}
int main() {
    try { filesAndFolders(); branchingAndFailures(); deletions(); deletionCleanup(); batches(); std::cout << "Explorer history tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
