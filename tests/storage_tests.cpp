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
    return a.top == b.top && a.left == b.left && a.anchor == b.anchor && a.caret == b.caret && a.after == b.after && a.inlineView == b.inlineView;
}
bool same(const poppy::ProjectSession& a, const poppy::ProjectSession& b) {
    if (a.sidebar != b.sidebar || a.terminalHeight != b.terminalHeight || a.terminalVisible != b.terminalVisible ||
        a.gitSidebar != b.gitSidebar || a.activeTab != b.activeTab || a.expanded != b.expanded || a.selected != b.selected || a.tabs.size() != b.tabs.size()) return false;
    for (size_t i = 0; i < a.tabs.size(); ++i) {
        const auto& x = a.tabs[i]; const auto& y = b.tabs[i];
        if (x.path != y.path || x.diff != y.diff || !same(x.fileView, y.fileView) || !same(x.diffView, y.diffView)) return false;
    }
    return true;
}
void roundTripAndIsolation() {
    Fixture fixture;
    const std::string key = "d:\\repository\\yumayo\\poppyworktrees\\poppydirect2d";
    const std::string path = "D:\\repository\\yumayo\\PoppyWorktrees\\PoppyDirect2D";
    poppy::ProjectSession state;
    state.sidebar = 333; state.terminalHeight = 222; state.terminalVisible = false; state.gitSidebar = true; state.activeTab = 1;
    state.tabs = {{path + "\\日本語's file.cpp", false, {42, 17, 30, 50, false, false}, {20, 3, 2, 11, true, true}},
                  {path + "\\deleted.cpp", true, {5, 2, 1, 7, false, true}, {50, 4, 6, 14, true, false}}};
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
    auto changed = original; changed.sidebar = 500; changed.tabs[0].path = "reject"; changed.expanded.clear();
    rejects([&] { storage.saveProject(key, key, changed); }, "A failed SQLite write reported success");
    check(same(*storage.loadProject(key, key), original), "A failed save partially overwrote the previous session");
    changed.tabs.resize(10001);
    rejects([&] { storage.saveProject(key, key, changed); }, "Oversized sessions were silently truncated");
    check(same(*storage.loadProject(key, key), original), "An oversized session overwrote previous state");
}
void schemaAndRegistry() {
    Fixture fixture;
    const auto appDb = fixture.directory / "storage.sql";
    {
        poppy::Storage storage(fixture.directory);
        const auto projectDb = storage.projectDirectory("c:\\future", "c:\\future") / "storage.sql";
        execute(projectDb, "PRAGMA user_version=2");
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
        roundTripAndIsolation(); atomicSave(); schemaAndRegistry();
        std::cout << "Storage tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
