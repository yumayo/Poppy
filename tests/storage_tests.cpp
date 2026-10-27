#include "storage.h"
#include "resource.h"
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
    return a.top == b.top && a.left == b.left && a.anchor == b.anchor && a.caret == b.caret && a.after == b.after && a.wrapOffset == b.wrapOffset;
}
bool same(const poppy::ProjectSession& a, const poppy::ProjectSession& b) {
    if (a.wslUid != b.wslUid) return false;
    if (a.sidebar != b.sidebar || a.terminalHeight != b.terminalHeight || a.terminalVisible != b.terminalVisible ||
        a.gitSidebar != b.gitSidebar || a.branchSidebar != b.branchSidebar || a.diffInline != b.diffInline || a.gitTree != b.gitTree || a.activeTab != b.activeTab || a.expanded != b.expanded || a.selected != b.selected || a.tabs.size() != b.tabs.size() || a.explorerTop != b.explorerTop || a.explorerLeft != b.explorerLeft ||
        a.gitTop != b.gitTop || a.gitLeft != b.gitLeft || a.firstTabRow != b.firstTabRow || a.rightTabRow != b.rightTabRow || a.editorRatio != b.editorRatio || a.terminalRatio != b.terminalRatio || a.gitSelected != b.gitSelected ||
        a.focusedPane != b.focusedPane || a.activeTerminal != b.activeTerminal || a.collapsedGit != b.collapsedGit || a.terminals.size() != b.terminals.size()) return false;
    for (size_t i = 0; i < a.tabs.size(); ++i) {
        const auto& x = a.tabs[i]; const auto& y = b.tabs[i];
        if (x.path != y.path || x.diff != y.diff || x.pinned != y.pinned || x.group != y.group || x.displayed != y.displayed || x.diffKind != y.diffKind || x.revision != y.revision || x.allChanges != y.allChanges || x.gitRoot != y.gitRoot || !same(x.fileView, y.fileView) || !same(x.diffView, y.diffView)) return false;
    }
    for (size_t i = 0; i < a.terminals.size(); ++i) { const auto& x = a.terminals[i]; const auto& y = b.terminals[i]; if (x.id != y.id || x.name != y.name || x.directory != y.directory || x.group != y.group || x.displayed != y.displayed) return false; }
    return true;
}
void roundTripAndIsolation() {
    Fixture fixture;
    const std::string key = "d:\\repository\\yumayo\\poppyworktrees\\poppydirect2d";
    const std::string path = "D:\\repository\\yumayo\\PoppyWorktrees\\PoppyDirect2D";
    poppy::ProjectSession state;
    state.sidebar = 333; state.terminalHeight = 222; state.terminalVisible = false; state.gitSidebar = true; state.activeTab = 1;
    state.diffInline = true; state.gitTree = true;
    state.explorerTop = 13; state.explorerLeft = 7; state.gitTop = 3; state.gitLeft = 4; state.firstTabRow = 2;
    state.gitSelected = "src/日本語"; state.collapsedGit = {"docs", "src/nested"}; state.focusedPane = "terminal";
    state.terminals = {{"codex", "Codex", path}, {"claude", "Claude Code", path}, {"shell", "確認用", path}}; state.activeTerminal = "claude";
    state.tabs = {{path + "\\日本語's file.cpp", false, {42, 17, 30, 50, false}, {20, 3, 2, 11, true}},
                  {path + "\\deleted.cpp", true, {5, 2, 1, 7, false}, {50, 4, 6, 14, true}}};
    state.tabs[0].pinned = true; state.tabs[0].displayed = true; state.tabs[1].group = 1; state.tabs[1].displayed = true;
    state.tabs[0].fileView.wrapOffset = 137;
    state.tabs[1].diffView.wrapOffset = 89;
    state.editorRatio = 350; state.terminalRatio = 640; state.rightTabRow = 4; state.terminals[1].group = 1; state.terminals[1].displayed = true;
    state.expanded = {path, path + "\\src", path + "\\src\\日本語"}; state.selected = state.tabs.front().path;
    poppy::AppSession app;
    app.lastProject = path; app.openProjects = {path, "D:\\other"}; app.openProjectKeys = {key, "d:\\other"}; app.projectListWidth = 233; app.x = -1500; app.y = 120; app.width = 1200; app.height = 800; app.positioned = true; app.maximized = true;
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
              savedApp.height == app.height && savedApp.openProjects == app.openProjects && savedApp.openProjectKeys == app.openProjectKeys && savedApp.projectListWidth == app.projectListWidth && savedApp.positioned && savedApp.maximized, "App state did not survive reopening");
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
        empty.diffInline = false; empty.gitTree = false;
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
    auto changed = original; changed.sidebar = 500; changed.diffInline = false; changed.gitTree = false; changed.tabs[0].path = "reject"; changed.tabs[0].pinned = true; changed.expanded.clear();
    rejects([&] { storage.saveProject(key, key, changed); }, "A failed SQLite write reported success");
    check(same(*storage.loadProject(key, key), original), "A failed save partially overwrote the previous session");
    changed.tabs.resize(10001);
    rejects([&] { storage.saveProject(key, key, changed); }, "Oversized sessions were silently truncated");
    check(same(*storage.loadProject(key, key), original), "An oversized session overwrote previous state");
}
void projectHistory() {
    Fixture fixture;
    const std::string key = "c:\\work\\日本語 project", path = "C:\\work\\日本語 project";
    const auto ubuntu = poppy::ResourceId::wsl("Ubuntu", "/home/user/project", "user");
    const auto debian = poppy::ResourceId::wsl("Debian", "/home/user/project", "user");
    poppy::ProjectSession state; state.sidebar = 321; state.terminals[0].directory = path;
    {
        poppy::Storage storage(fixture.directory);
        check(storage.projectHistory().empty(), "New storage has project history");
        storage.loadProject(key, path);
        storage.saveProject(key, path, state);
        storage.loadProject(ubuntu.key(), ubuntu.serialized());
        storage.loadProject(debian.key(), debian.serialized());
        poppy::AppSession app; app.openProjects = {path}; app.openProjectKeys = {key};
        storage.saveApp(app);
        // Closing the last project must only clear the currently open list.
        storage.saveApp({});
    }
    {
        poppy::Storage storage(fixture.directory);
        check(storage.loadApp().openProjects.empty(), "Closing projects retained the open list");
        const std::vector<std::string> expected{debian.serialized(), ubuntu.serialized(), path};
        check(storage.projectHistory() == expected, "Closed Windows/WSL history was lost or duplicated after restart");
        check(same(*storage.loadProject(key, path), state), "Reopening from history lost the saved project session");
        check(storage.projectHistory() == expected, "Reopening a project duplicated its history");
        // History also works for projects saved before this feature existed:
        // it reads the registry and does not require a new app-state write.
        execute(fixture.directory / "storage.sql", "DELETE FROM app;");
        check(storage.projectHistory() == expected, "History depended on the open workspace or app settings");
    }
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
    original.sidebar = 321; original.gitSidebar = true; original.diffInline = true; original.gitTree = false;
    original.tabs.push_back({key + "\\modified.cpp", true, {}, {12, 3, 4, 5, true}});
    original.expanded = {key}; original.selected = key;
    storage.saveProject(key, key, original);
    execute(storage.projectDirectory(key, key) / "storage.sql",
        "DROP TABLE split_layout; DROP TABLE tab_groups; DROP TABLE terminal_groups; DROP TABLE view_state; DROP TABLE terminals; DROP TABLE git_collapsed; ALTER TABLE session DROP COLUMN git_tree; ALTER TABLE tabs DROP COLUMN pinned; ALTER TABLE tabs DROP COLUMN file_wrap_offset; ALTER TABLE tabs DROP COLUMN diff_wrap_offset; ALTER TABLE git_comparisons DROP COLUMN repository; PRAGMA user_version=2;");
    const auto saved = storage.loadProject(key, key);
    original.terminals[0].directory = key;
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
        "DROP TABLE split_layout; DROP TABLE tab_groups; DROP TABLE terminal_groups; DROP TABLE view_state; DROP TABLE terminals; DROP TABLE git_collapsed; ALTER TABLE tabs DROP COLUMN pinned; ALTER TABLE tabs DROP COLUMN file_wrap_offset; ALTER TABLE tabs DROP COLUMN diff_wrap_offset; ALTER TABLE git_comparisons DROP COLUMN repository; PRAGMA user_version=3;");
    const auto saved = storage.loadProject(key, key);
    original.terminals[0].directory = key;
    check(saved && same(*saved, original), "Migrating version 3 changed existing tabs or pinned them");
    auto changed = *saved; changed.tabs[0].pinned = true;
    storage.saveProject(key, key, changed);
    check(same(*storage.loadProject(key, key), changed), "A migrated project lost a pinned tab");
    changed.tabs[0].pinned = false;
    storage.saveProject(key, key, changed);
    check(same(*storage.loadProject(key, key), original), "Unpinning a tab was not saved");
}
void migrateWorkspaceAndTerminals() {
    Fixture fixture;
    const auto appDb = fixture.directory / "storage.sql";
    execute(appDb,
        "CREATE TABLE projects(path_key TEXT PRIMARY KEY,path TEXT NOT NULL,directory TEXT NOT NULL UNIQUE);"
        "CREATE TABLE app(id INTEGER PRIMARY KEY,last_project TEXT NOT NULL,x INTEGER NOT NULL,y INTEGER NOT NULL,width INTEGER NOT NULL,height INTEGER NOT NULL,positioned INTEGER NOT NULL,maximized INTEGER NOT NULL);"
        "INSERT INTO app VALUES(1,'C:\\selected',20,30,1200,800,1,0);"
        "INSERT INTO projects VALUES('c:\\recent','C:\\recent','-c-recent'); PRAGMA user_version=1;");
    poppy::Storage storage(fixture.directory);
    const auto app = storage.loadApp();
    check(app.openProjects == std::vector<std::string>{"C:\\selected"} && app.projectListWidth == 180 && app.x == 20, "App migration opened recent projects or lost placement");
    check(storage.projectHistory() == std::vector<std::string>{"C:\\recent"}, "Legacy closed project was missing from history");
    const std::string key = "c:\\terminals";
    poppy::ProjectSession original;
    original.terminalHeight = 333; original.terminalVisible = false;
    storage.saveProject(key, key, original);
    execute(storage.projectDirectory(key,key)/"storage.sql", "DROP TABLE split_layout; DROP TABLE tab_groups; DROP TABLE terminal_groups; DROP TABLE view_state; DROP TABLE terminals; DROP TABLE git_collapsed; ALTER TABLE tabs DROP COLUMN file_wrap_offset; ALTER TABLE tabs DROP COLUMN diff_wrap_offset; ALTER TABLE git_comparisons DROP COLUMN repository; PRAGMA user_version=4;");
    auto saved = storage.loadProject(key,key);
    check(saved && saved->terminals.size() == 1 && saved->terminals[0].directory == key && saved->terminalHeight == 333 && !saved->terminalVisible, "Version 4 migration did not preserve terminal layout");
    auto changed = *saved; changed.terminals.push_back(changed.terminals.front());
    rejects([&]{storage.saveProject(key,key,changed);}, "Duplicate terminal IDs accepted");
    check(same(*storage.loadProject(key,key),*saved), "Failed terminal save changed prior state");
    auto duplicate = app; duplicate.openProjects.push_back(app.openProjects.front());
    rejects([&]{storage.saveApp(duplicate);}, "Duplicate open projects accepted");
    check(storage.loadApp().openProjects == app.openProjects, "Failed app save changed the open list");
}
void migrateSplitLayout() {
    Fixture fixture; poppy::Storage storage(fixture.directory); const std::string key = "c:/split-migration";
    poppy::ProjectSession original; original.tabs = {{key + "/a.txt", false, {}, {}}, {key + "/b.txt", false, {}, {}}};
    storage.saveProject(key, key, original);
    execute(storage.projectDirectory(key,key)/"storage.sql", "DROP TABLE split_layout; DROP TABLE tab_groups; DROP TABLE terminal_groups; ALTER TABLE tabs DROP COLUMN file_wrap_offset; ALTER TABLE tabs DROP COLUMN diff_wrap_offset; ALTER TABLE git_comparisons DROP COLUMN repository; PRAGMA user_version=5;");
    const auto restored = storage.loadProject(key,key);
    check(restored && same(*restored, original), "Version 5 migration lost the tabs or default layout");
}
void migrateWrapping() {
    Fixture fixture; poppy::Storage storage(fixture.directory);
    const std::string key = "c:/wrapped-tabs";
    poppy::ProjectSession state;
    state.tabs.push_back({key + "/long.txt", false, {12, 0, 30, 50, false}, {}});
    storage.saveProject(key, key, state);
    execute(storage.projectDirectory(key, key) / "storage.sql", "ALTER TABLE tabs DROP COLUMN file_wrap_offset; ALTER TABLE tabs DROP COLUMN diff_wrap_offset; ALTER TABLE git_comparisons DROP COLUMN repository; PRAGMA user_version=8;");
    check(same(*storage.loadProject(key, key), state), "Version 8 migration changed the top logical line or selection");
    state.tabs[0].fileView.wrapOffset = 243;
    storage.saveProject(key, key, state);
    check(same(*storage.loadProject(key, key), state), "A migrated tab lost its wrapped scroll position");
    execute(storage.projectDirectory(key, key) / "storage.sql", "ALTER TABLE tabs DROP COLUMN diff_wrap_offset; ALTER TABLE git_comparisons DROP COLUMN repository; PRAGMA user_version=9;");
    check(same(*storage.loadProject(key, key), state), "Version 9 migration changed a file's wrapped scroll position");
    state.tabs[0].diffView = {8, 0, 30, 50, true, 113};
    storage.saveProject(key, key, state);
    check(same(*storage.loadProject(key, key), state), "A migrated diff lost its continuation or selected side");
}
void schemaAndRegistry() {
    Fixture fixture;
    const auto appDb = fixture.directory / "storage.sql";
    {
        poppy::Storage storage(fixture.directory);
        const auto projectDb = storage.projectDirectory("c:\\future", "c:\\future") / "storage.sql";
        execute(projectDb, "PRAGMA user_version=12");
        rejects([&] { storage.loadProject("c:\\future", "c:\\future"); }, "A newer project schema was overwritten");
        storage.projectDirectory("c:\\invalid", "c:\\invalid");
        execute(appDb, R"(UPDATE projects SET directory='../../escape' WHERE path_key='c:\invalid')");
        rejects([&] { storage.projectDirectory("c:\\invalid", "c:\\invalid"); }, "An invalid registry path escaped the project directory");
    }
    execute(appDb, "PRAGMA user_version=4");
    rejects([&] { poppy::Storage storage(fixture.directory); }, "A newer app schema was overwritten");
}
void gitComparisonRoundTrip() {
    Fixture fixture; poppy::Storage storage(fixture.directory);
    const std::string key = "c:/git-comparisons";
    poppy::ProjectSession state; state.branchSidebar = true; state.focusedPane = "branch";
    // The same path may be open as a file, staged diff, worktree diff and two
    // historical comparisons, independently of its editor group.
    for (const auto kind : {poppy::GitDiffKind::Automatic, poppy::GitDiffKind::Changes, poppy::GitDiffKind::Staged, poppy::GitDiffKind::Commit, poppy::GitDiffKind::Commit}) {
        poppy::SavedTab tab; tab.path = key + "/file.txt"; tab.diff = kind != poppy::GitDiffKind::Automatic; tab.diffKind = kind;
        tab.revision = kind == poppy::GitDiffKind::Commit ? (state.tabs.size() == 3 ? "first" : "second") : "";
        tab.gitRoot = state.tabs.size() % 2 ? key + "/app-client" : key + "/app-server";
        tab.group = state.tabs.size() % 2; tab.displayed = state.tabs.size() == 3 || state.tabs.size() == 4;
        state.tabs.push_back(tab);
    }
    auto all = state.tabs.back(); all.path = key; all.allChanges = true; all.displayed = false; state.tabs.push_back(all);
    state.activeTab = 4;
    storage.saveProject(key, key, state);
    check(same(*storage.loadProject(key, key), state), "Git comparisons sharing a path lost their scopes, revisions, groups or selection");
    // A version 10 project gains repository ownership without changing tabs.
    for (auto& tab : state.tabs) tab.gitRoot.clear();
    storage.saveProject(key, key, state);
    execute(storage.projectDirectory(key, key) / "storage.sql", "ALTER TABLE git_comparisons DROP COLUMN repository; PRAGMA user_version=10;");
    check(same(*storage.loadProject(key, key), state), "Repository migration changed existing comparisons");
    // A version 7 project gains comparison metadata without changing tabs.
    state = {}; state.tabs.push_back({key + "/legacy.txt", true, {}, {3, 2, 0, 0, true}});
    storage.saveProject(key, key, state);
    execute(storage.projectDirectory(key, key) / "storage.sql", "DROP TABLE git_comparisons; ALTER TABLE tabs DROP COLUMN file_wrap_offset; ALTER TABLE tabs DROP COLUMN diff_wrap_offset; PRAGMA user_version=7;");
    check(same(*storage.loadProject(key, key), state), "Version 7 migration changed existing file diff tabs");
}
void wslRoundTripAndMigration() {
    Fixture fixture;
    const auto windows = std::string("C:\\project");
    fs::path oldDirectory;
    {
        poppy::Storage storage(fixture.directory); poppy::ProjectSession local; local.sidebar = 301;
        storage.saveProject("c:\\project", windows, local); oldDirectory = storage.projectDirectory("c:\\project",windows);
        execute(oldDirectory/"storage.sql","DROP TABLE connection; ALTER TABLE tabs DROP COLUMN file_wrap_offset; ALTER TABLE tabs DROP COLUMN diff_wrap_offset; ALTER TABLE git_comparisons DROP COLUMN repository; PRAGMA user_version=6;");
    }
    execute(fixture.directory/"storage.sql","ALTER TABLE projects DROP COLUMN kind; ALTER TABLE projects DROP COLUMN distro; ALTER TABLE projects DROP COLUMN user; ALTER TABLE projects DROP COLUMN remote_root; PRAGMA user_version=2;");
    {
        poppy::Storage storage(fixture.directory);
        check(storage.projectDirectory("c:\\project",windows) == oldDirectory && storage.loadProject("c:\\project",windows)->sidebar == 301,"WSL migration changed an existing Windows project");
        const auto first = poppy::ResourceId::wsl("Ubuntu","/home/user/project"), second = poppy::ResourceId::wsl("Debian","/home/user/project"), anotherUser = poppy::ResourceId::wsl("Ubuntu","/home/user/project","other");
        poppy::ProjectSession state; state.wslUid = "1000"; state.tabs = {{"/home/user/project/A.cpp",false,{},{}},{"/home/user/project/a.cpp",false,{},{}}}; state.tabs[0].displayed = true; state.tabs[1].group = 1; state.tabs[1].displayed = true; state.editorRatio = 425;
        state.expanded = {first.u8string(), first.u8string()+"/src"}; state.terminals = {{"left","Linux",first.u8string(),0,true},{"right","Linux",first.u8string()+"/src",1,true}}; state.activeTerminal = "left";
        storage.saveProject(first.key(),first.serialized(),state); storage.saveProject(second.key(),second.serialized(),{}); storage.saveProject(anotherUser.key(),anotherUser.serialized(),{});
        const auto directory = storage.projectDirectory(first.key(),first.serialized());
        check(directory.filename().string().rfind("wsl-ubuntu-",0) == 0 && directory != oldDirectory && directory != storage.projectDirectory(second.key(),second.serialized()) && directory != storage.projectDirectory(anotherUser.key(),anotherUser.serialized()),"WSL storage authorities collided");
        check(same(*storage.loadProject(first.key(),first.serialized()),state),"WSL UID, case-sensitive tabs, split or POSIX cwd was lost");
        poppy::AppSession app; app.openProjects = {windows,first.serialized(),second.serialized()}; app.openProjectKeys = {"c:\\project",first.key(),second.key()}; app.lastProject = first.serialized(); storage.saveApp(app);
        const auto loaded = storage.loadApp(); check(loaded.openProjects == app.openProjects && loaded.openProjectKeys == app.openProjectKeys && loaded.lastProject == app.lastProject,"Mixed Windows/WSL workspace was not restored");
        auto invalid = state; invalid.terminals.push_back(invalid.terminals[0]); rejects([&] { storage.saveProject(first.key(),first.serialized(),invalid); },"Invalid WSL transaction was accepted"); check(same(*storage.loadProject(first.key(),first.serialized()),state),"Failed WSL save overwrote previous state");
    }
}
}
int main() {
    try {
        roundTripAndIsolation(); atomicSave(); projectHistory(); migrateProjectLayout(); migrateGitLayout(); migratePinnedTabs(); migrateWorkspaceAndTerminals(); migrateSplitLayout(); migrateWrapping(); schemaAndRegistry(); gitComparisonRoundTrip(); wslRoundTripAndMigration();
        std::cout << "Storage tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
