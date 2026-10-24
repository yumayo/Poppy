#include "storage.h"
#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace poppy {
namespace {
namespace fs = std::filesystem;
constexpr size_t maxSavedTabs = 10000, maxSavedDirectories = 50000;

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
void schema(Database& db, const char* sql) {
    // Refuse newer schemas rather than changing a database from a newer app.
    {
        Statement version(db, "PRAGMA user_version");
        if (version.row()) {
            if (version.number(0) > 1) throw std::runtime_error("This state database requires a newer version of Poppy");
            if (version.number(0) == 1) return;
        }
    }
    Transaction transaction(db);
    db.exec(sql); db.exec("PRAGMA user_version=1"); transaction.commit();
}
int bounded(sqlite3_int64 value, int low, int high) { return static_cast<int>(std::clamp(value, static_cast<sqlite3_int64>(low), static_cast<sqlite3_int64>(high))); }
size_t position(sqlite3_int64 value) { return static_cast<size_t>(std::clamp(value, sqlite3_int64{0}, sqlite3_int64{16 * 1024 * 1024})); }
void bindView(Statement& statement, int start, const ViewState& state) {
    statement.number(start, state.top); statement.number(start + 1, state.left);
    statement.number(start + 2, static_cast<sqlite3_int64>(state.anchor)); statement.number(start + 3, static_cast<sqlite3_int64>(state.caret));
    statement.number(start + 4, state.after); statement.number(start + 5, state.inlineView);
}
ViewState readView(const Statement& statement, int start) {
    ViewState state;
    state.top = bounded(statement.number(start), 0, 1000000); state.left = bounded(statement.number(start + 1), 0, 8192);
    state.anchor = position(statement.number(start + 2)); state.caret = position(statement.number(start + 3));
    state.after = statement.number(start + 4) != 0; state.inlineView = statement.number(start + 5) != 0;
    return state;
}
void projectSchema(Database& db) {
    schema(db,
        "CREATE TABLE IF NOT EXISTS session (id INTEGER PRIMARY KEY CHECK(id=1), sidebar INTEGER NOT NULL, terminal_height INTEGER NOT NULL,"
        " terminal_visible INTEGER NOT NULL, git_sidebar INTEGER NOT NULL, active_tab INTEGER NOT NULL, selected_path TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS tabs (position INTEGER PRIMARY KEY, path TEXT NOT NULL, diff INTEGER NOT NULL,"
        " file_top INTEGER NOT NULL, file_left INTEGER NOT NULL, file_anchor INTEGER NOT NULL, file_caret INTEGER NOT NULL, file_after INTEGER NOT NULL, file_inline INTEGER NOT NULL,"
        " diff_top INTEGER NOT NULL, diff_left INTEGER NOT NULL, diff_anchor INTEGER NOT NULL, diff_caret INTEGER NOT NULL, diff_after INTEGER NOT NULL, diff_inline INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS expanded (path TEXT PRIMARY KEY);");
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
        "CREATE TABLE IF NOT EXISTS projects (path_key TEXT PRIMARY KEY, path TEXT NOT NULL, directory TEXT NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS app (id INTEGER PRIMARY KEY CHECK(id=1), last_project TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL,"
        " width INTEGER NOT NULL, height INTEGER NOT NULL, positioned INTEGER NOT NULL, maximized INTEGER NOT NULL);");
}
Storage::~Storage() = default;
AppSession Storage::loadApp() {
    AppSession state;
    Statement row(*impl_->app, "SELECT last_project,x,y,width,height,positioned,maximized FROM app WHERE id=1");
    if (row.row()) {
        state.lastProject = row.text(0); state.x = bounded(row.number(1), -1000000, 1000000); state.y = bounded(row.number(2), -1000000, 1000000);
        state.width = bounded(row.number(3), 720, 30000); state.height = bounded(row.number(4), 480, 30000);
        state.positioned = row.number(5) != 0; state.maximized = row.number(6) != 0;
    }
    return state;
}
void Storage::saveApp(const AppSession& state) {
    Statement row(*impl_->app, "INSERT OR REPLACE INTO app VALUES(1,?,?,?,?,?,?,?)");
    row.text(1, state.lastProject); row.number(2, state.x); row.number(3, state.y);
    row.number(4, state.width); row.number(5, state.height); row.number(6, state.positioned); row.number(7, state.maximized); row.run();
}
fs::path Storage::projectDirectory(const std::string& key, const std::string& path) {
    if (key.empty()) throw std::runtime_error("Cannot store a project without a path");
    std::string name;
    {
        Transaction transaction(*impl_->app);
        Statement existing(*impl_->app, "SELECT directory FROM projects WHERE path_key=?"); existing.text(1, key);
        if (existing.row()) name = existing.text(0);
        else {
            const auto base = projectDirectoryName(key);
            name = base;
            // The readable name is intentionally lossy. Resolve collisions in
            // the registry before assigning a directory, including hash ties.
            for (size_t attempt = 0;; ++attempt) {
                Statement collision(*impl_->app, "SELECT 1 FROM projects WHERE directory=?"); collision.text(1, name);
                if (!collision.row() && !fs::exists(impl_->directory / "projects" / name)) break;
                name = base + '-' + projectPathHash(key);
                if (attempt) name += '-' + std::to_string(attempt);
            }
            Statement insert(*impl_->app, "INSERT INTO projects VALUES(?,?,?)");
            insert.text(1, key); insert.text(2, path); insert.text(3, name); insert.run();
        }
        transaction.commit();
    }
    // Registry values must remain a single component below projects/.
    if (name.empty() || name.front() != '-' || name.find_first_not_of("-abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
        throw std::runtime_error("Invalid project directory in state database");
    const auto directory = impl_->directory / "projects" / name;
    fs::create_directories(directory);
    return directory;
}
std::optional<ProjectSession> Storage::loadProject(const std::string& key, const std::string& path) {
    Database db(projectDirectory(key, path) / "storage.sql"); projectSchema(db);
    Transaction snapshot(db, false);
    ProjectSession state;
    {
        Statement row(db, "SELECT sidebar,terminal_height,terminal_visible,git_sidebar,active_tab,selected_path FROM session WHERE id=1");
        if (!row.row()) return std::nullopt;
        state.sidebar = bounded(row.number(0), 130, 10000); state.terminalHeight = bounded(row.number(1), 100, 10000);
        state.terminalVisible = row.number(2) != 0; state.gitSidebar = row.number(3) != 0;
        state.activeTab = static_cast<size_t>(bounded(row.number(4), 0, static_cast<int>(maxSavedTabs - 1))); state.selected = row.text(5);
    }
    Statement tabs(db, "SELECT path,diff,file_top,file_left,file_anchor,file_caret,file_after,file_inline,diff_top,diff_left,diff_anchor,diff_caret,diff_after,diff_inline FROM tabs ORDER BY position LIMIT 10000");
    while (tabs.row()) state.tabs.push_back({tabs.text(0), tabs.number(1) != 0, readView(tabs, 2), readView(tabs, 8)});
    Statement expanded(db, "SELECT path FROM expanded ORDER BY path LIMIT 50000");
    while (expanded.row()) state.expanded.push_back(expanded.text(0));
    return state;
}
void Storage::saveProject(const std::string& key, const std::string& path, const ProjectSession& state) {
    if (state.tabs.size() > maxSavedTabs || state.expanded.size() > maxSavedDirectories) throw std::runtime_error("Too many items to save the project session");
    Database db(projectDirectory(key, path) / "storage.sql"); projectSchema(db);
    Transaction transaction(db);
    db.exec("DELETE FROM tabs; DELETE FROM expanded;");
    Statement session(db, "INSERT OR REPLACE INTO session VALUES(1,?,?,?,?,?,?)");
    session.number(1, state.sidebar); session.number(2, state.terminalHeight); session.number(3, state.terminalVisible);
    session.number(4, state.gitSidebar); session.number(5, static_cast<sqlite3_int64>(state.activeTab)); session.text(6, state.selected); session.run();
    Statement tab(db, "INSERT INTO tabs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    for (size_t i = 0; i < state.tabs.size(); ++i) {
        const auto& value = state.tabs[i];
        tab.number(1, static_cast<sqlite3_int64>(i)); tab.text(2, value.path); tab.number(3, value.diff);
        bindView(tab, 4, value.fileView); bindView(tab, 10, value.diffView); tab.run(); tab.reset();
    }
    Statement expanded(db, "INSERT OR IGNORE INTO expanded VALUES(?)");
    for (const auto& value : state.expanded) { expanded.text(1, value); expanded.run(); expanded.reset(); }
    transaction.commit();
}
}
