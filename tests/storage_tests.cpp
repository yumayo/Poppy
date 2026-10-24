#include "storage.h"
#include <sqlite3.h>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char* message) {
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
struct Fixture {
    fs::path directory;
    Fixture() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        directory = fs::temp_directory_path() / fs::u8path("Poppy storage 日本語 " + std::to_string(stamp));
        check(fs::create_directory(directory), "Cannot create test directory");
    }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
};
void execute(const fs::path& path, const char* sql) {
    sqlite3* db = nullptr;
    const int opened = sqlite3_open(path.u8string().c_str(), &db);
    if (opened != SQLITE_OK) { if (db) sqlite3_close(db); throw std::runtime_error("Cannot open test database"); }
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    const std::string error = sqlite3_errmsg(db); sqlite3_close(db);
    if (result != SQLITE_OK) throw std::runtime_error(error);
}
bool same(const poppy::ViewState& a, const poppy::ViewState& b) {
    return a.top == b.top && a.left == b.left && a.anchor == b.anchor && a.caret == b.caret && a.after == b.after;
}
bool same(const poppy::ProjectSession& a, const poppy::ProjectSession& b) {
    if (a.sidebar != b.sidebar || a.terminalHeight != b.terminalHeight || a.terminalVisible != b.terminalVisible ||
        a.gitSidebar != b.gitSidebar || a.diffInline != b.diffInline || a.gitTree != b.gitTree || a.activeTab != b.activeTab || a.expanded != b.expanded || a.selected != b.selected || a.tabs.size() != b.tabs.size()) return false;
    for (size_t i = 0; i < a.tabs.size(); ++i) {
        const auto& x = a.tabs[i]; const auto& y = b.tabs[i];
        if (x.path != y.path || x.diff != y.diff || x.pinned != y.pinned || !same(x.fileView, y.fileView) || !same(x.diffView, y.diffView)) return false;
    }
    return true;
}
void roundTripAndIsolation() {
    Fixture fixture;
    const std::string key = "d:\\repository\\yumayo\\poppyworktrees\\poppydirect2d";
    const std::string path = "D:\\repository\\yumayo\\PoppyWorktrees\\PoppyDirect2D";
    poppy::ProjectSession state;
    state.sidebar = 333; state.terminalHeight = 222; state.terminalVisible = false; state.gitSidebar = true; state.activeTab = 1;
    state.diffInline = true; state.gitTree = true;
    state.tabs = {{path + "\\日本語's file.cpp", false, {42, 17, 30, 50, false}, {20, 3, 2, 11, true}},
                  {path + "\\deleted.cpp", true, {5, 2, 1, 7, false}, {50, 4, 6, 14, true}}};
    state.tabs[0].pinned = true;
    state.expanded = {path, path + "\\src", path + "\\src\\日本語"}; state.selected = state.tabs.front().path;
    poppy::AppSession app;
    app.lastProject = path; app.x = -1500; app.y = 120; app.width = 1200; app.height = 800; app.positioned = true; app.maximized = true;
    fs::path projectDirectory;
    {
        poppy::Storage storage(fixture.directory);
        check(storage.loadApp().lastProject.empty(), "A new app database has a previous project");
        check(!storage.loadProject(key, path), "A new project has a saved session");
        projectDirectory = storage.projectDirectory(key, path);
        check(projectDirectory == fixture.directory / "projects" / "-d-repository-yumayo-poppyworktrees-poppydirect2d", "Unexpected project storage directory");
        storage.saveApp(app); storage.saveProject(key, path, state);
        check(fs::is_regular_file(fixture.directory / "storage.sql") && fs::is_regular_file(projectDirectory / "storage.sql"), "State databases were not created");
    }
    {
        poppy::Storage storage(fixture.directory);
        const auto savedApp = storage.loadApp();
        check(savedApp.lastProject == app.lastProject && savedApp.x == app.x && savedApp.y == app.y && savedApp.width == app.width &&
              savedApp.height == app.height && savedApp.positioned && savedApp.maximized, "App state did not survive reopening");
        auto saved = storage.loadProject(key, path);
        check(saved && same(*saved, state), "Project state did not survive reopening");
        const std::string a = "d:\\a.b", b = "d:\\a-b";
        const auto first = storage.projectDirectory(a, a), second = storage.projectDirectory(b, b);
        check(first != second && first.filename() == "-d-a-b", "Readable path collisions share a directory");
        check(second == storage.projectDirectory(b, b), "Collision directory assignment is unstable");
        const auto orphan = fixture.directory / "projects" / "-d-orphan";
        fs::create_directory(orphan);
        check(storage.projectDirectory("d:\\orphan", "d:\\orphan") != orphan, "An unregistered existing directory was reused");
        storage.saveProject(a, a, state);
        poppy::ProjectSession empty; empty.terminalVisible = false;
        storage.saveProject(b, b, empty);
        check(same(*storage.loadProject(a, a), state) && same(*storage.loadProject(b, b), empty), "One project overwrote another");
        storage.saveProject(key, path, empty);
        check(same(*storage.loadProject(key, path), empty), "Closed tabs or collapsed directories were retained");
        empty.diffInline = true; empty.gitTree = true;
        storage.saveProject(key, path, empty);
        check(same(*storage.loadProject(key, path), empty), "A project's layout was lost when no files were open");
    }
}
void atomicSave() {
    Fixture fixture;
    poppy::Storage storage(fixture.directory);
    const std::string key = "c:\\atomic";
    poppy::ProjectSession original; original.tabs.push_back({"c:\\atomic\\original.txt", false, {}, {}}); original.expanded = {key};
    storage.saveProject(key, key, original);
    execute(storage.projectDirectory(key, key) / "storage.sql",
        "CREATE TRIGGER reject_tab BEFORE INSERT ON tabs WHEN NEW.path='reject' BEGIN SELECT RAISE(ABORT,'test write failure'); END;");
    auto changed = original; changed.sidebar = 500; changed.diffInline = true; changed.gitTree = true; changed.tabs[0].path = "reject"; changed.tabs[0].pinned = true; changed.expanded.clear();
    rejects([&] { storage.saveProject(key, key, changed); }, "A failed SQLite write reported success");
    check(same(*storage.loadProject(key, key), original), "A failed save partially overwrote the previous session");
    changed.tabs.resize(10001);
    rejects([&] { storage.saveProject(key, key, changed); }, "Oversized sessions were silently truncated");
    check(same(*storage.loadProject(key, key), original), "An oversized session overwrote previous state");
}
void migrateProjectLayout() {
    Fixture fixture;
    poppy::Storage storage(fixture.directory);
    for (int active : {0, 2}) {
        const std::string key = "c:\\legacy-" + std::to_string(active);
        const auto db = storage.projectDirectory(key, key) / "storage.sql";
        execute(db,
            "CREATE TABLE session (id INTEGER PRIMARY KEY CHECK(id=1), sidebar INTEGER NOT NULL, terminal_height INTEGER NOT NULL,"
            " terminal_visible INTEGER NOT NULL, git_sidebar INTEGER NOT NULL, active_tab INTEGER NOT NULL, selected_path TEXT NOT NULL);"
            "CREATE TABLE tabs (position INTEGER PRIMARY KEY, path TEXT NOT NULL, diff INTEGER NOT NULL,"
            " file_top INTEGER NOT NULL, file_left INTEGER NOT NULL, file_anchor INTEGER NOT NULL, file_caret INTEGER NOT NULL, file_after INTEGER NOT NULL, file_inline INTEGER NOT NULL,"
            " diff_top INTEGER NOT NULL, diff_left INTEGER NOT NULL, diff_anchor INTEGER NOT NULL, diff_caret INTEGER NOT NULL, diff_after INTEGER NOT NULL, diff_inline INTEGER NOT NULL);"
            "CREATE TABLE expanded (path TEXT PRIMARY KEY);"
            "INSERT INTO session VALUES(1,333,222,0,1,0,'selected');"
            "INSERT INTO tabs VALUES(0,'normal',0,42,17,30,50,0,1,20,3,2,11,1,0);"
            "INSERT INTO tabs VALUES(1,'split',1,5,2,1,7,0,0,50,4,6,14,1,0);"
            "INSERT INTO tabs VALUES(2,'inline',1,5,2,1,7,0,0,30,4,6,14,1,1);"
            "INSERT INTO expanded VALUES('expanded');"
            "PRAGMA user_version=1;");
        if (active == 2) execute(db, "UPDATE session SET active_tab=2;");
        const auto saved = storage.loadProject(key, key);
        check(saved && !saved->gitTree, "Migration did not preserve the Git list layout");
        check(saved && saved->diffInline == (active == 2), "Migration did not prefer the active diff or first diff tab's layout");
        check(saved->activeTab == static_cast<size_t>(active) && saved->sidebar == 333 && saved->terminalHeight == 222 &&
              !saved->terminalVisible && saved->gitSidebar && saved->selected == "selected" && saved->expanded == std::vector<std::string>{"expanded"},
              "Migration lost project settings");
        check(saved->tabs.size() == 3 && saved->tabs[0].path == "normal" && !saved->tabs[0].diff &&
              same(saved->tabs[0].fileView, {42, 17, 30, 50, false}) && same(saved->tabs[1].diffView, {0, 4, 6, 14, true}) &&
              same(saved->tabs[2].diffView, {30, 4, 6, 14, true}), "Migration lost tab state or retained incompatible split row indices");
        for (const auto& tab : saved->tabs) check(!tab.pinned, "Migration pinned a legacy tab");
        rejects([&] { execute(db, "SELECT file_inline FROM tabs;"); }, "Normal tabs still store a layout setting");
        rejects([&] { execute(db, "SELECT diff_inline FROM tabs;"); }, "Diff tabs still store a layout setting");
        check(same(*storage.loadProject(key, key), *saved), "Reopening repeated the migration");
        auto changed = *saved; changed.diffInline = !saved->diffInline; changed.gitTree = true;
        storage.saveProject(key, key, changed);
        check(same(*storage.loadProject(key, key), changed), "Migrated project layouts cannot be updated");
    }
}
void migrateGitLayout() {
    Fixture fixture;
    poppy::Storage storage(fixture.directory);
    const std::string key = "c:\\git-layout";
    poppy::ProjectSession original;
    original.sidebar = 321; original.gitSidebar = true; original.diffInline = true;
    original.tabs.push_back({key + "\\modified.cpp", true, {}, {12, 3, 4, 5, true}});
    original.expanded = {key}; original.selected = key;
    storage.saveProject(key, key, original);
    execute(storage.projectDirectory(key, key) / "storage.sql",
        "ALTER TABLE session DROP COLUMN git_tree; ALTER TABLE tabs DROP COLUMN pinned; PRAGMA user_version=2;");
    const auto saved = storage.loadProject(key, key);
    check(saved && same(*saved, original), "Migrating version 2 changed existing project state");
    auto changed = *saved; changed.gitTree = true;
    storage.saveProject(key, key, changed);
    check(same(*storage.loadProject(key, key), changed), "A migrated Git view cannot be saved");
}
void migratePinnedTabs() {
    Fixture fixture;
    poppy::Storage storage(fixture.directory);
    const std::string key = "c:\\pinned-tabs";
    poppy::ProjectSession original;
    original.activeTab = 1; original.gitTree = true;
    original.tabs = {{key + "\\first.cpp", false, {42, 17, 30, 50, false}, {}},
                     {key + "\\second.cpp", true, {}, {12, 3, 4, 5, true}}};
    storage.saveProject(key, key, original);
    execute(storage.projectDirectory(key, key) / "storage.sql",
        "ALTER TABLE tabs DROP COLUMN pinned; PRAGMA user_version=3;");
    const auto saved = storage.loadProject(key, key);
    check(saved && same(*saved, original), "Migrating version 3 changed existing tabs or pinned them");
    auto changed = *saved; changed.tabs[0].pinned = true;
    storage.saveProject(key, key, changed);
    check(same(*storage.loadProject(key, key), changed), "A migrated project lost a pinned tab");
    changed.tabs[0].pinned = false;
    storage.saveProject(key, key, changed);
    check(same(*storage.loadProject(key, key), original), "Unpinning a tab was not saved");
}
void schemaAndRegistry() {
    Fixture fixture;
    const auto appDb = fixture.directory / "storage.sql";
    {
        poppy::Storage storage(fixture.directory);
        const auto projectDb = storage.projectDirectory("c:\\future", "c:\\future") / "storage.sql";
        execute(projectDb, "PRAGMA user_version=5");
        rejects([&] { storage.loadProject("c:\\future", "c:\\future"); }, "A newer project schema was overwritten");
        storage.projectDirectory("c:\\invalid", "c:\\invalid");
        execute(appDb, R"(UPDATE projects SET directory='../../escape' WHERE path_key='c:\invalid')");
        rejects([&] { storage.projectDirectory("c:\\invalid", "c:\\invalid"); }, "An invalid registry path escaped the project directory");
    }
    execute(appDb, "PRAGMA user_version=2");
    rejects([&] { poppy::Storage storage(fixture.directory); }, "A newer app schema was overwritten");
}
}
int main() {
    try {
        roundTripAndIsolation(); atomicSave(); migrateProjectLayout(); migrateGitLayout(); migratePinnedTabs(); schemaAndRegistry();
        std::cout << "Storage tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
