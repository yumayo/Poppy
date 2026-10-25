#include "storage.h"
#include "resource.h"
#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace poppy {
namespace {
namespace fs = std::filesystem;
constexpr size_t maxSavedTabs = 10000, maxSavedDirectories = 50000;
constexpr const char* projectViews =
    "CREATE TABLE view_state (id INTEGER PRIMARY KEY CHECK(id=1), explorer_top INTEGER NOT NULL, explorer_left INTEGER NOT NULL,"
    " git_top INTEGER NOT NULL, git_left INTEGER NOT NULL, first_tab_row INTEGER NOT NULL, git_selected TEXT NOT NULL, focused_pane TEXT NOT NULL, active_terminal TEXT NOT NULL);"
    "CREATE TABLE terminals (position INTEGER PRIMARY KEY, terminal_id TEXT NOT NULL UNIQUE, name TEXT NOT NULL, directory TEXT NOT NULL);"
    "CREATE TABLE git_collapsed (path TEXT PRIMARY KEY);";
constexpr const char* splitViews =
    "CREATE TABLE split_layout (id INTEGER PRIMARY KEY CHECK(id=1), editor_ratio INTEGER NOT NULL, terminal_ratio INTEGER NOT NULL, right_tab_row INTEGER NOT NULL);"
    "CREATE TABLE tab_groups (path TEXT PRIMARY KEY, pane INTEGER NOT NULL, displayed INTEGER NOT NULL);"
    "CREATE TABLE terminal_groups (terminal_id TEXT PRIMARY KEY, pane INTEGER NOT NULL, displayed INTEGER NOT NULL);";
constexpr const char* projectConnection =
    "CREATE TABLE IF NOT EXISTS connection (id INTEGER PRIMARY KEY CHECK(id=1), kind INTEGER NOT NULL, distro TEXT NOT NULL, user TEXT NOT NULL, remote_root TEXT NOT NULL, uid TEXT NOT NULL);";
constexpr const char* appLocations =
    "ALTER TABLE projects ADD COLUMN kind INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE projects ADD COLUMN distro TEXT NOT NULL DEFAULT '';"
    "ALTER TABLE projects ADD COLUMN user TEXT NOT NULL DEFAULT '';"
    "ALTER TABLE projects ADD COLUMN remote_root TEXT NOT NULL DEFAULT '';";

class Database {
public:
    explicit Database(const fs::path& path) {
        const int result = sqlite3_open_v2(path.u8string().c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (result != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "Cannot allocate SQLite connection";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error(path.u8string() + ": " + error);
        }
        sqlite3_busy_timeout(db_, 1000);
    }
    ~Database() { sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    sqlite3* get() const { return db_; }
    void exec(const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK) fail();
    }
    [[noreturn]] void fail() const { throw std::runtime_error(sqlite3_errmsg(db_)); }
private:
    sqlite3* db_ = nullptr;
};
class Statement {
public:
    Statement(Database& db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db.get(), sql, -1, &statement_, nullptr) != SQLITE_OK) db.fail();
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void text(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) db_.fail();
    }
    void number(int index, sqlite3_int64 value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) db_.fail();
    }
    bool row() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result != SQLITE_DONE) db_.fail();
        return false;
    }
    void run() { if (row()) throw std::runtime_error("Unexpected SQLite result"); }
    void reset() { if (sqlite3_reset(statement_) != SQLITE_OK) db_.fail(); }
    std::string text(int column) const {
        const auto value = sqlite3_column_text(statement_, column);
        return value ? std::string(reinterpret_cast<const char*>(value), static_cast<size_t>(sqlite3_column_bytes(statement_, column))) : std::string{};
    }
    sqlite3_int64 number(int column) const { return sqlite3_column_int64(statement_, column); }
private:
    Database& db_;
    sqlite3_stmt* statement_ = nullptr;
};
class Transaction {
public:
    explicit Transaction(Database& db, bool write = true) : db_(db) { db_.exec(write ? "BEGIN IMMEDIATE" : "BEGIN"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_.get(), "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { db_.exec("COMMIT"); committed_ = true; }
private:
    Database& db_;
    bool committed_ = false;
};
void schema(Database& db, const char* sql, std::initializer_list<const char*> upgrades = {}) {
    const auto currentVersion = static_cast<sqlite3_int64>(upgrades.size() + 1);
    // Refuse newer schemas rather than changing a database from a newer app.
    sqlite3_int64 savedVersion = 0;
    {
        Statement version(db, "PRAGMA user_version");
        if (version.row()) savedVersion = version.number(0);
    }
    if (savedVersion > currentVersion) throw std::runtime_error("This state database requires a newer version of Poppy");
    if (savedVersion == currentVersion) return;
    Transaction transaction(db);
    if (savedVersion == 0) db.exec(sql);
    else for (auto version = savedVersion; version < currentVersion; ++version) db.exec(upgrades.begin()[version - 1]);
    db.exec(("PRAGMA user_version=" + std::to_string(currentVersion)).c_str()); transaction.commit();
}
int bounded(sqlite3_int64 value, int low, int high) { return static_cast<int>(std::clamp(value, static_cast<sqlite3_int64>(low), static_cast<sqlite3_int64>(high))); }
size_t position(sqlite3_int64 value) { return static_cast<size_t>(std::clamp(value, sqlite3_int64{0}, sqlite3_int64{16 * 1024 * 1024})); }
void bindView(Statement& statement, int start, const ViewState& state) {
    statement.number(start, state.top); statement.number(start + 1, state.left);
    statement.number(start + 2, static_cast<sqlite3_int64>(state.anchor)); statement.number(start + 3, static_cast<sqlite3_int64>(state.caret));
    statement.number(start + 4, state.after);
}
ViewState readView(const Statement& statement, int start) {
    ViewState state;
    state.top = bounded(statement.number(start), 0, 1000000); state.left = bounded(statement.number(start + 1), 0, 8192);
    state.anchor = position(statement.number(start + 2)); state.caret = position(statement.number(start + 3));
    state.after = statement.number(start + 4) != 0;
    return state;
}
void projectSchema(Database& db) {
    schema(db,
        (std::string("CREATE TABLE IF NOT EXISTS session (id INTEGER PRIMARY KEY CHECK(id=1), sidebar INTEGER NOT NULL, terminal_height INTEGER NOT NULL,"
        " terminal_visible INTEGER NOT NULL, git_sidebar INTEGER NOT NULL, active_tab INTEGER NOT NULL, selected_path TEXT NOT NULL, diff_inline INTEGER NOT NULL, git_tree INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS tabs (position INTEGER PRIMARY KEY, path TEXT NOT NULL, diff INTEGER NOT NULL,"
        " file_top INTEGER NOT NULL, file_left INTEGER NOT NULL, file_anchor INTEGER NOT NULL, file_caret INTEGER NOT NULL, file_after INTEGER NOT NULL,"
        " diff_top INTEGER NOT NULL, diff_left INTEGER NOT NULL, diff_anchor INTEGER NOT NULL, diff_caret INTEGER NOT NULL, diff_after INTEGER NOT NULL, pinned INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS expanded (path TEXT PRIMARY KEY);") + projectViews + splitViews + projectConnection).c_str(), {
        "ALTER TABLE session ADD COLUMN diff_inline INTEGER NOT NULL DEFAULT 0;"
        "UPDATE session SET diff_inline=COALESCE((SELECT diff_inline FROM tabs WHERE diff<>0"
        " ORDER BY (position=(SELECT active_tab FROM session WHERE id=1)) DESC,position LIMIT 1),0);"
        // Old split row indices cannot be converted without the original patch.
        "UPDATE tabs SET diff_top=0 WHERE diff_inline=0;"
        "ALTER TABLE tabs DROP COLUMN file_inline;"
        "ALTER TABLE tabs DROP COLUMN diff_inline;",
        "ALTER TABLE session ADD COLUMN git_tree INTEGER NOT NULL DEFAULT 0;",
        "ALTER TABLE tabs ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0;", projectViews, splitViews, projectConnection});
}
}
struct Storage::Impl {
    fs::path directory;
    std::unique_ptr<Database> app;
};
Storage::Storage(fs::path directory) : impl_(std::make_unique<Impl>()) {
    impl_->directory = std::move(directory);
    fs::create_directories(impl_->directory / "projects");
    impl_->app = std::make_unique<Database>(impl_->directory / "storage.sql");
    schema(*impl_->app,
        "CREATE TABLE IF NOT EXISTS projects (path_key TEXT PRIMARY KEY, path TEXT NOT NULL, directory TEXT NOT NULL UNIQUE, kind INTEGER NOT NULL DEFAULT 0, distro TEXT NOT NULL DEFAULT '', user TEXT NOT NULL DEFAULT '', remote_root TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE IF NOT EXISTS app (id INTEGER PRIMARY KEY CHECK(id=1), last_project TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL,"
        " width INTEGER NOT NULL, height INTEGER NOT NULL, positioned INTEGER NOT NULL, maximized INTEGER NOT NULL, project_list_width INTEGER NOT NULL DEFAULT 180);"
        "CREATE TABLE open_projects (path_key TEXT PRIMARY KEY, path TEXT NOT NULL, position INTEGER NOT NULL UNIQUE);", {
        "ALTER TABLE app ADD COLUMN project_list_width INTEGER NOT NULL DEFAULT 180;"
        "CREATE TABLE open_projects (path_key TEXT PRIMARY KEY, path TEXT NOT NULL, position INTEGER NOT NULL UNIQUE);"
        "INSERT INTO open_projects SELECT COALESCE((SELECT path_key FROM projects WHERE path=last_project),lower(last_project)),last_project,0 FROM app WHERE last_project<>'';", appLocations});
}
Storage::~Storage() = default;
AppSession Storage::loadApp() {
    AppSession state;
    Transaction snapshot(*impl_->app, false);
    Statement row(*impl_->app, "SELECT last_project,x,y,width,height,positioned,maximized,project_list_width FROM app WHERE id=1");
    if (row.row()) {
        state.lastProject = row.text(0); state.x = bounded(row.number(1), -1000000, 1000000); state.y = bounded(row.number(2), -1000000, 1000000);
        state.width = bounded(row.number(3), 720, 30000); state.height = bounded(row.number(4), 480, 30000);
        state.positioned = row.number(5) != 0; state.maximized = row.number(6) != 0;
        state.projectListWidth = bounded(row.number(7), 100, 600);
    }
    Statement projects(*impl_->app, "SELECT path_key,path FROM open_projects ORDER BY position LIMIT 1000");
    while (projects.row()) { state.openProjectKeys.push_back(projects.text(0)); state.openProjects.push_back(projects.text(1)); }
    snapshot.commit();
    return state;
}
void Storage::saveApp(const AppSession& state) {
    if (state.openProjects.size() > 1000 || (!state.openProjectKeys.empty() && state.openProjectKeys.size() != state.openProjects.size())) throw std::runtime_error("Invalid open project list");
    Transaction transaction(*impl_->app);
    Statement row(*impl_->app, "INSERT OR REPLACE INTO app VALUES(1,?,?,?,?,?,?,?,?)");
    row.text(1, state.lastProject); row.number(2, state.x); row.number(3, state.y);
    row.number(4, state.width); row.number(5, state.height); row.number(6, state.positioned); row.number(7, state.maximized); row.number(8, state.projectListWidth); row.run();
    impl_->app->exec("DELETE FROM open_projects");
    Statement project(*impl_->app, "INSERT INTO open_projects VALUES(?,?,?)");
    for (size_t i = 0; i < state.openProjects.size(); ++i) {
        auto key = state.openProjectKeys.empty() ? state.openProjects[i] : state.openProjectKeys[i];
        if (state.openProjectKeys.empty()) std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c); });
        if (key.empty()) throw std::runtime_error("Cannot store an empty project key");
        project.text(1, key); project.text(2, state.openProjects[i]); project.number(3, static_cast<sqlite3_int64>(i)); project.run(); project.reset();
    }
    transaction.commit();
}
fs::path Storage::projectDirectory(const std::string& key, const std::string& path) {
    if (key.empty()) throw std::runtime_error("Cannot store a project without a path");
    std::string name;
    {
        Transaction transaction(*impl_->app);
        Statement existing(*impl_->app, "SELECT directory FROM projects WHERE path_key=?"); existing.text(1, key);
        if (existing.row()) name = existing.text(0);
        else {
            const auto resource = ResourceId::parse(path);
            const auto base = resource.wsl() ? "wsl" + projectDirectoryName(resource.location().distro).substr(0, 40) + projectDirectoryName(resource.u8string()).substr(0, 60) + '-' + projectPathHash(key) : projectDirectoryName(key);
            name = base;
            // The readable name is intentionally lossy. Resolve collisions in
            // the registry before assigning a directory, including hash ties.
            for (size_t attempt = 0;; ++attempt) {
                Statement collision(*impl_->app, "SELECT 1 FROM projects WHERE directory=?"); collision.text(1, name);
                if (!collision.row() && !fs::exists(impl_->directory / "projects" / name)) break;
                name = base + '-' + projectPathHash(key);
                if (attempt) name += '-' + std::to_string(attempt);
            }
            Statement insert(*impl_->app, "INSERT INTO projects(path_key,path,directory,kind,distro,user,remote_root) VALUES(?,?,?,?,?,?,?)");
            insert.text(1, key); insert.text(2, path); insert.text(3, name); insert.number(4, resource.wsl()); insert.text(5, resource.location().distro); insert.text(6, resource.location().user); insert.text(7, resource.wsl() ? resource.u8string() : ""); insert.run();
        }
        transaction.commit();
    }
    // Registry values must remain a single component below projects/.
    if (name.empty() || (name.front() != '-' && name.rfind("wsl-", 0) != 0) || name.find_first_not_of("-abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
        throw std::runtime_error("Invalid project directory in state database");
    const auto directory = impl_->directory / "projects" / name;
    fs::create_directories(directory);
    return directory;
}
std::optional<ProjectSession> Storage::loadProject(const std::string& key, const std::string& path) {
    Database db(projectDirectory(key, path) / "storage.sql"); projectSchema(db);
    Transaction snapshot(db, false);
    ProjectSession state;
    Statement identity(db, "SELECT uid FROM connection WHERE id=1");
    if (identity.row()) state.wslUid = identity.text(0);
    {
        Statement row(db, "SELECT sidebar,terminal_height,terminal_visible,git_sidebar,active_tab,selected_path,diff_inline,git_tree FROM session WHERE id=1");
        if (!row.row()) return std::nullopt;
        state.sidebar = bounded(row.number(0), 130, 10000); state.terminalHeight = bounded(row.number(1), 100, 10000);
        state.terminalVisible = row.number(2) != 0; state.gitSidebar = row.number(3) != 0;
        state.activeTab = static_cast<size_t>(bounded(row.number(4), 0, static_cast<int>(maxSavedTabs - 1))); state.selected = row.text(5);
        state.diffInline = row.number(6) != 0;
        state.gitTree = row.number(7) != 0;
    }
    Statement tabs(db, "SELECT path,diff,file_top,file_left,file_anchor,file_caret,file_after,diff_top,diff_left,diff_anchor,diff_caret,diff_after,pinned FROM tabs ORDER BY position LIMIT 10000");
    while (tabs.row()) state.tabs.push_back({tabs.text(0), tabs.number(1) != 0, readView(tabs, 2), readView(tabs, 7), tabs.number(12) != 0});
    Statement expanded(db, "SELECT path FROM expanded ORDER BY path LIMIT 50000");
    while (expanded.row()) state.expanded.push_back(expanded.text(0));
    Statement views(db, "SELECT explorer_top,explorer_left,git_top,git_left,first_tab_row,git_selected,focused_pane,active_terminal FROM view_state WHERE id=1");
    if (views.row()) {
        state.explorerTop = bounded(views.number(0), 0, 50000); state.explorerLeft = bounded(views.number(1), 0, 1000000);
        state.gitTop = bounded(views.number(2), 0, 50000); state.gitLeft = bounded(views.number(3), 0, 1000000);
        state.firstTabRow = static_cast<size_t>(bounded(views.number(4), 0, 10000));
        state.gitSelected = views.text(5); state.focusedPane = views.text(6); state.activeTerminal = views.text(7);
    }
    Statement terminals(db, "SELECT terminal_id,name,directory FROM terminals ORDER BY position LIMIT 100");
    state.terminals.clear();
    while (terminals.row()) state.terminals.push_back({terminals.text(0), terminals.text(1), terminals.text(2)});
    if (state.terminals.empty()) state.terminals.push_back({"shell-1", "Shell", path});
    if (std::none_of(state.terminals.begin(), state.terminals.end(), [&](const SavedTerminal& terminal) { return terminal.id == state.activeTerminal; })) state.activeTerminal = state.terminals.front().id;
    Statement collapsed(db, "SELECT path FROM git_collapsed ORDER BY path LIMIT 50000");
    while (collapsed.row()) state.collapsedGit.push_back(collapsed.text(0));
    Statement layout(db, "SELECT editor_ratio,terminal_ratio,right_tab_row FROM split_layout WHERE id=1");
    if (layout.row()) { state.editorRatio = bounded(layout.number(0), 200, 800); state.terminalRatio = bounded(layout.number(1), 200, 800); state.rightTabRow = position(layout.number(2)); }
    Statement groups(db, "SELECT path,pane,displayed FROM tab_groups");
    while (groups.row()) for (auto& tab : state.tabs) if (tab.path == groups.text(0)) { tab.group = bounded(groups.number(1), 0, 1); tab.displayed = groups.number(2) != 0; break; }
    Statement terminalGroups(db, "SELECT terminal_id,pane,displayed FROM terminal_groups");
    while (terminalGroups.row()) for (auto& terminal : state.terminals) if (terminal.id == terminalGroups.text(0)) { terminal.group = bounded(terminalGroups.number(1), 0, 1); terminal.displayed = terminalGroups.number(2) != 0; break; }
    snapshot.commit();
    return state;
}
void Storage::saveProject(const std::string& key, const std::string& path, const ProjectSession& state) {
    if (state.tabs.size() > maxSavedTabs || state.expanded.size() > maxSavedDirectories || state.collapsedGit.size() > maxSavedDirectories || state.terminals.size() > 100) throw std::runtime_error("Too many items to save the project session");
    Database db(projectDirectory(key, path) / "storage.sql"); projectSchema(db);
    Transaction transaction(db);
    const auto resource = ResourceId::parse(path);
    Statement connection(db, "INSERT OR REPLACE INTO connection VALUES(1,?,?,?,?,?)");
    connection.number(1, resource.wsl()); connection.text(2, resource.location().distro); connection.text(3, resource.location().user); connection.text(4, resource.wsl() ? resource.u8string() : ""); connection.text(5, state.wslUid); connection.run();
    db.exec("DELETE FROM tabs; DELETE FROM expanded;");
    Statement session(db, "INSERT OR REPLACE INTO session VALUES(1,?,?,?,?,?,?,?,?)");
    session.number(1, state.sidebar); session.number(2, state.terminalHeight); session.number(3, state.terminalVisible);
    session.number(4, state.gitSidebar); session.number(5, static_cast<sqlite3_int64>(state.activeTab)); session.text(6, state.selected);
    session.number(7, state.diffInline); session.number(8, state.gitTree); session.run();
    Statement tab(db, "INSERT INTO tabs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    for (size_t i = 0; i < state.tabs.size(); ++i) {
        const auto& value = state.tabs[i];
        tab.number(1, static_cast<sqlite3_int64>(i)); tab.text(2, value.path); tab.number(3, value.diff);
        bindView(tab, 4, value.fileView); bindView(tab, 9, value.diffView); tab.number(14, value.pinned); tab.run(); tab.reset();
    }
    Statement expanded(db, "INSERT OR IGNORE INTO expanded VALUES(?)");
    for (const auto& value : state.expanded) { expanded.text(1, value); expanded.run(); expanded.reset(); }
    db.exec("DELETE FROM terminals; DELETE FROM git_collapsed;");
    Statement views(db, "INSERT OR REPLACE INTO view_state VALUES(1,?,?,?,?,?,?,?,?)");
    views.number(1, state.explorerTop); views.number(2, state.explorerLeft); views.number(3, state.gitTop); views.number(4, state.gitLeft);
    views.number(5, static_cast<sqlite3_int64>(state.firstTabRow)); views.text(6, state.gitSelected); views.text(7, state.focusedPane); views.text(8, state.activeTerminal); views.run();
    Statement terminal(db, "INSERT INTO terminals VALUES(?,?,?,?)");
    for (size_t i = 0; i < state.terminals.size(); ++i) {
        const auto& value = state.terminals[i];
        if (value.id.empty() || value.id.size() > 64 || value.name.size() > 512) throw std::runtime_error("Invalid terminal configuration");
        terminal.number(1, static_cast<sqlite3_int64>(i)); terminal.text(2, value.id); terminal.text(3, value.name); terminal.text(4, value.directory); terminal.run(); terminal.reset();
    }
    Statement collapsed(db, "INSERT OR IGNORE INTO git_collapsed VALUES(?)");
    for (const auto& value : state.collapsedGit) { collapsed.text(1, value); collapsed.run(); collapsed.reset(); }
    db.exec("DELETE FROM tab_groups; DELETE FROM terminal_groups;");
    Statement layout(db, "INSERT OR REPLACE INTO split_layout VALUES(1,?,?,?)");
    layout.number(1, state.editorRatio); layout.number(2, state.terminalRatio); layout.number(3, static_cast<sqlite3_int64>(state.rightTabRow)); layout.run();
    Statement groups(db, "INSERT INTO tab_groups VALUES(?,?,?)");
    for (const auto& tab : state.tabs) { groups.text(1, tab.path); groups.number(2, tab.group); groups.number(3, tab.displayed); groups.run(); groups.reset(); }
    Statement terminalGroups(db, "INSERT INTO terminal_groups VALUES(?,?,?)");
    for (const auto& terminal : state.terminals) { terminalGroups.text(1, terminal.id); terminalGroups.number(2, terminal.group); terminalGroups.number(3, terminal.displayed); terminalGroups.run(); terminalGroups.reset(); }
    transaction.commit();
}
}
