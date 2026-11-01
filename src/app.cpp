#include "viewer.h"
#include "terminal_pane.h"
#include "project_runtime.h"
#include "tree.h"
#include "git_changes.h"
#include "git_history.h"
#include "git_ignore.h"
#include "git_commit_pane.h"
#include "activity_button.h"
#include "quick_open.h"
#include "search_pane.h"
#include "file_name.h"
#include "clipboard_image.h"
#include "project_provider.h"
#include "file_changes.h"
#include "explorer_history.h"
#include "explorer_clipboard.h"
#include "status_bar.h"
#include "editor_tabs.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

namespace poppy {
namespace {
constexpr UINT_PTR timerId = 1;
constexpr UINT_PTR tabDragTimerId = 2;
constexpr size_t maxEntries = 10000, maxTreeNodes = 50000, maxLines = 1000000;
constexpr int explorerCommand = 2, gitCommand = 3, searchCommand = 7;
enum class Sidebar { Explorer, Git, Search };
using Entry = ProviderEntry;
struct Node {
    ResourceId path; uint64_t id;
    bool directory, loaded = false, loading = false, reparse = false;
    // Keep expansion intent while loading, even when an empty item cannot expand.
    bool expandRequested = false;
    bool unsupported = false, truncated = false, ignored = false; std::wstring label;
};
bool explorerLess(uintptr_t a, uintptr_t b) {
    const auto x = reinterpret_cast<const Node*>(a), y = reinterpret_cast<const Node*>(b);
    if (x->directory != y->directory) return x->directory;
    const auto left = x->unsupported ? x->label : x->path.filename().native();
    const auto right = y->unsupported ? y->label : y->path.filename().native();
    // Use Windows' case-insensitive natural order for both Windows and WSL.
    const int order = StrCmpLogicalW(left.c_str(), right.c_str());
    if (order) return order < 0;
    // WSL can contain distinct names that differ only in case or zero padding.
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, FALSE) == CSTR_LESS_THAN;
}
struct DirectoryResult : ProviderListing { std::unordered_set<std::string> ignored; };

std::unordered_set<std::string> ignoredNames(ProjectProvider& provider, const ResourceId& directory, const std::vector<std::string>& names,
    const std::atomic<bool>* cancelled) {
    return gitIgnoredNames(names, [&](const std::vector<std::string>& arguments) -> std::optional<std::string> {
        std::vector<std::wstring> args;
        for (const auto& arg : arguments) args.push_back(wide(arg));
        const auto result = provider.git(args, directory, cancelled);
        if (result.code > 1) return std::nullopt;
        // Process-launch/timeout failures also use code 1, with an error.
        if (result.code == 1 && !result.error.empty()) return std::nullopt;
        return result.output;
    });
}
struct FileResult { std::shared_ptr<EditBuffer> buffer; std::shared_ptr<Document> document; std::wstring error; std::shared_ptr<const ImagePreview> image; };
using Tab = EditorTab;
std::wstring tabKey(const Tab& tab) {
    return pathKey(tab.path) + L"\n" + std::to_wstring(static_cast<int>(tab.diffKind)) + L"\n" + wide(tab.revision) + (tab.allChanges ? L"\nall" : L"") + (tab.gitRoot.empty() ? L"" : L"\n" + pathKey(tab.gitRoot));
}
std::wstring tabTitle(const Tab& tab) {
    auto title = tab.allChanges ? L"All Changes · " + tab.path.filename().native() : tab.path.filename().native();
    if (tab.diffKind == GitDiffKind::Commit) title += L" · " + wide(tab.revision.substr(0, 8));
    else if (tab.diffKind == GitDiffKind::Staged) title += L" · Staged";
    else if (tab.diffKind != GitDiffKind::Automatic) title += L" · Changes";
    return title + (tab.diff ? L" (diff)" : L"");
}
struct TabDrop { size_t position; RECT marker; int group = 0; };
struct EditorGroup { RECT tabs{}, body{}; std::vector<size_t> indices, starts; size_t firstRow = 0, rows = 0; bool overflow() const { return starts.size() > rows; } };
struct TabDrag {
    size_t index;
    POINT origin, point;
    bool active = false;
    std::optional<TabDrop> target;
};
struct GitEntry { ResourceId path; wchar_t state; bool staged = false, changed = false; char index = ' ', worktree = ' '; std::string previousPath; };
struct GitResult { ResourceId root; std::wstring branch, error; std::vector<GitEntry> entries; size_t changed = 0, untracked = 0; bool truncated = false, unsupportedNames = false; };
struct GitWorkspace {
    GitRepositoryScan scan;
    std::vector<std::shared_ptr<GitResult>> repositories;
    std::vector<ResourceId> roots;
    std::map<std::wstring, wchar_t> states;
    std::wstring error, warning;
    size_t changed = 0, untracked = 0;
    bool truncated = false, unsupportedNames = false;
    const GitResult* repository(const ResourceId& path) const {
        const auto root = gitRepositoryFor(roots, path);
        for (const auto& repo : repositories) if (pathKey(repo->root) == pathKey(root)) return repo.get();
        return nullptr;
    }
};
struct SourceNode { std::wstring path; size_t entry = noGitChange; bool directory = false, staged = false, group = false; size_t repository = 0; };
enum class HistoryKind { Branch, Commit, Directory, File, More, Message, Repository, Source };
struct HistoryNode {
    HistoryKind kind; std::string ref, revision; ResourceId path;
    bool loaded = false, loading = false; size_t count = 0;
    uint64_t generation = 0;
    ResourceId root = {};
    bool current = false;
    SourceNode source = {};
    std::vector<GitCommitFile> files = {};
};
std::vector<std::wstring> gitArguments(const std::vector<std::string>& args) {
    std::vector<std::wstring> result; result.reserve(args.size());
    for (const auto& arg : args) result.push_back(wide(arg));
    return result;
}

COLORREF stateColor(wchar_t state) {
    return state == L'U' || state == L'D' ? RGB(231,112,118) : state == L'?' || state == L'A' ? accent : RGB(224,192,128);
}

std::shared_ptr<FileResult> readDocument(ProjectProvider& provider, const ResourceId& path) {
    auto result = std::make_shared<FileResult>();
    auto file = provider.readFile(path);
    if (!file.error.empty()) { result->error = file.error; return result; }
    if (isImageExtension(path.extension().native())) {
        try { result->image = decodeImage(file.bytes); }
        catch (const std::exception& error) { result->error = wide(error.what()); }
        return result;
    }
    auto bytes = std::move(file.bytes); auto buffer = std::make_shared<EditBuffer>(); auto document = std::shared_ptr<Document>(buffer, &buffer->document); std::string error;
    if (!decodeText(bytes, document->text, document->encoding, error)) { result->error = wide(error); return result; }
    if (static_cast<size_t>(std::count(document->text.begin(), document->text.end(), '\n')) >= maxLines) { result->error = L"This file exceeds the 1,000,000-line preview limit."; return result; }
    document->language = path.filename() == L"CMakeLists.txt" ? Language::Config : languageFor(utf8(path.extension().native()));
    indexDocument(*document); buffer->initialize(std::move(bytes)); buffer->fileVersion = std::move(file.version); result->buffer = std::move(buffer); result->document = std::move(document); return result;
}
std::shared_ptr<Document> readWorkingBase(ProjectProvider& provider, const ResourceId& root, const std::string& relative, Language language, const std::atomic<bool>* cancelled) {
    const auto entries = provider.git({L"git.exe", L"--literal-pathspecs", L"--no-optional-locks", L"ls-files", L"--stage", L"-z", L"--", wide(relative)}, root, cancelled);
    if (entries.code) return {};
    std::string object;
    for (size_t at = 0; at < entries.output.size();) {
        const auto end = entries.output.find('\0', at);
        if (end == std::string::npos) return {};
        const auto entry = entries.output.substr(at, end - at); at = end + 1;
        const auto space = entry.find(' '), stage = entry.find(' ', space + 1), tab = entry.find('\t', stage + 1);
        if (space == entry.npos || stage == entry.npos || tab == entry.npos) return {};
        if (entry.substr(stage + 1, tab - stage - 1) != "0" && entry.substr(stage + 1, tab - stage - 1) != "2") continue;
        if (entry.substr(0, space) != "100644" && entry.substr(0, space) != "100755") return {};
        object = entry.substr(space + 1, stage - space - 1); break;
    }
    auto document = std::make_shared<Document>(); document->language = language;
    if (!object.empty()) {
        if (object.find_first_not_of("0123456789abcdef") != object.npos) return {};
        const auto blob = provider.git({L"git.exe", L"--no-optional-locks", L"cat-file", L"blob", wide(object)}, root, cancelled);
        std::string error;
        if (blob.code || !decodeText(blob.output, document->text, document->encoding, error) ||
            static_cast<size_t>(std::count(document->text.begin(), document->text.end(), '\n')) >= maxLines) return {};
    }
    indexDocument(*document); return document;
}
std::wstring trimmed(std::string text) { while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back(); return wide(text); }
std::shared_ptr<GitResult> readGit(ProjectProvider& provider, const ResourceId& directory, const std::atomic<bool>* cancelled) {
    auto result = std::make_shared<GitResult>();
    auto root = provider.git({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--show-toplevel"}, directory, cancelled);
    if (root.code) { if (root.code != 128 || root.output.find("not a git repository") == std::string::npos) result->error = root.error; return result; }
    if (directory.wsl() && !validUtf8(root.output)) { result->error = L"The Git repository root is not a UTF-8 path."; return result; }
    const auto repository = directory.at(utf8(trimmed(root.output)));
    // A removed/invalid .git candidate must not make Git walk up to an
    // unrelated parent worktree. Reject it before reading branch or status.
    if (!directory.contains(repository)) return result;
    result->root = repository;
    auto branch = provider.git({L"git.exe", L"--no-optional-locks", L"symbolic-ref", L"--short", L"-q", L"HEAD"}, result->root, cancelled);
    if (branch.code == 0) result->branch = trimmed(branch.output);
    else {
        auto head = provider.git({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--short", L"HEAD"}, result->root, cancelled);
        result->branch = head.code == 0 ? trimmed(head.output) + L" (detached)" : L"HEAD";
    }
    auto status = provider.git(gitArguments(gitStatusArguments()), result->root, cancelled);
    if (status.code) { result->error = status.error; return result; }
    for (const auto& entry : parseGitStatus(status.output)) {
        const char x = entry.index, y = entry.worktree;
        if (directory.wsl() && (!validUtf8(entry.path) || !validUtf8(entry.previousPath))) { result->unsupportedNames = true; continue; }
        const wchar_t state = x == '?' ? L'?' : x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D') ? L'U' : y != ' ' ? y : x;
        if (state == L'?') ++result->untracked; else ++result->changed;
        ResourceId path = (result->root / std::string_view(entry.path)).lexically_normal();
        if (result->entries.size() < maxTreeNodes) result->entries.push_back({path, state, entry.staged(), entry.changed(), x, y, entry.previousPath});
        else result->truncated = true;
    }
    return result;
}
std::shared_ptr<GitWorkspace> readGitWorkspace(ProjectProvider& provider, const ResourceId& directory, const std::atomic<bool>* cancelled, const GitRepositoryScan* cached) {
    auto result = std::make_shared<GitWorkspace>();
    result->scan = cached ? *cached : provider.gitRepositories(directory, [cancelled] { return cancelled && *cancelled; });
    const auto& scan = result->scan;
    result->warning = wide(scan.error); result->truncated = scan.truncated;
    // Only .git candidates in the opened folder belong to this workspace.
    // Running Git in the folder itself can resolve an ancestor outside it.
    for (const auto& candidate : scan.roots) {
        if (cancelled && *cancelled) return result;
        if (std::any_of(result->roots.begin(), result->roots.end(), [&](const ResourceId& root) { return pathKey(root) == pathKey(candidate); })) continue;
        if (result->repositories.size() >= 256) { result->truncated = true; break; }
        auto repo = readGit(provider, candidate, cancelled);
        if (repo->root.empty()) {
            if (repo->error.empty()) continue;
            repo->root = candidate;
        }
        if (std::any_of(result->roots.begin(), result->roots.end(), [&](const ResourceId& root) { return pathKey(root) == pathKey(repo->root); })) continue;
        result->roots.push_back(repo->root); result->repositories.push_back(std::move(repo));
    }
    if (!result->repositories.empty()) result->error = result->repositories.size() == 1 ? result->repositories.front()->error : std::wstring{};
    else if (result->error.empty()) result->error = result->warning;
    std::sort(result->repositories.begin(), result->repositories.end(), [](const auto& a, const auto& b) { return pathKey(a->root) < pathKey(b->root); });
    size_t remainingEntries = maxTreeNodes;
    for (const auto& repo : result->repositories) {
        // A nested worktree supplies its own untracked files. Keep tracked
        // entries in the parent, including submodule pointer changes.
        auto& entries = repo->entries;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const GitEntry& entry) {
            if (entry.state != L'?' || pathKey(gitRepositoryFor(result->roots, entry.path)) == pathKey(repo->root)) return false;
            --repo->untracked; return true;
        }), entries.end());
        result->changed += repo->changed; result->untracked += repo->untracked;
        if (entries.size() > remainingEntries) { entries.resize(remainingEntries); repo->truncated = true; }
        remainingEntries -= entries.size();
        result->truncated = result->truncated || repo->truncated;
        result->unsupportedNames = result->unsupportedNames || repo->unsupportedNames;
        for (const auto& entry : entries) {
            auto mark = [&](ResourceId path) {
                while (!path.empty()) {
                    auto found = result->states.find(pathKey(path));
                    if (found != result->states.end()) { if (entry.state == L'U') found->second = entry.state; }
                    else if (result->states.size() < maxTreeNodes) result->states[pathKey(path)] = entry.state;
                    else result->truncated = true;
                    if (path == directory || path == path.root_path() || (path == repo->root && !directory.contains(path))) break;
                    path = path.parent_path();
                }
            };
            mark(entry.path);
            if (!entry.previousPath.empty()) mark(repo->root / std::string_view(entry.previousPath));
        }
    }
    return result;
}
}

class ProjectRuntime::Impl {
    friend class ProjectRuntime;
public:
    bool key(const MSG& message);
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void create();
    void dpi(UINT value);
    void layout(bool revealTab = true);
    void paint();
    void selectSidebar(Sidebar sidebar);
    RECT gitViewButton(bool tree) const;
    void setGitView(bool tree);
    void updateGitFiles();
    void openGitFile(TreeItem item, bool diff = true);
    void openWorkingFile(const ResourceId& path);
    SourceNode* sourceNode(TreeItem item) const;
    void updateCommitFiles(TreeItem item);
    void clearHistoryChildren(TreeItem item);
    void sortHistory(TreeItem item);
    void stageGitChanges(TreeItem item, bool all = false);
    void discardGitChanges(TreeItem item, bool all = false);
    void stashGitChanges(TreeItem item);
    void commitGitChanges(const ResourceId& root, std::wstring message, bool amend);
    void openGitDiff(const ResourceId& path, GitDiffKind kind, std::string revision = {}, bool allChanges = false, ResourceId root = {});
    std::wstring repositoryLabel(const ResourceId& root) const;
    void refreshBranches();
    void requestHistory(TreeItem item, bool more = false);
    void openHistory(TreeItem item);
    HistoryNode* historyNode(TreeItem item) const;
    TreeItem insertHistory(TreeItem parent, std::wstring text, HistoryNode node, bool expandable, COLORREF color = foreground);
    void chooseFolder();
    void openFolder(ResourceId path);
    ProjectSession saveSession();
    void restoreProject(const ProjectSession& state);
    void forgetExpanded(const ResourceId& path);
    void requestDirectory(TreeItem item);
    void applyDirectory(TreeItem item, Node* node, const DirectoryResult& result);
    void revealActiveFile();
    void revealPendingFile();
    TreeItem insert(TreeItem parent, const Entry& entry);
    Node* node(TreeItem item) const;
    void updateLabel(TreeItem item);
    bool canCreateItem(TreeItem item) const;
    void newFile();
    void createItem(TreeItem parent, const std::wstring& name, bool directory);
    void renameFile(TreeItem item, const std::wstring& name);
    void deleteItems(const std::vector<TreeItem>& items);
    void copyItems(const std::vector<TreeItem>& items, bool cut);
    void pasteFiles(TreeItem item);
    void moveItems(const std::vector<TreeItem>& items, TreeItem parent);
    void executeExplorerBatch(std::vector<ExplorerEdit> edits, std::vector<ResourceId> copies = {}, ExplorerClipboard clipboard = {});
    void applyExplorerBatch(const std::vector<ExplorerEdit>& edits, bool redo);
    bool canMoveFile(TreeItem item, TreeItem parent) const;
    void relocateFile(TreeItem item, const ResourceId& destination, TreeItem parent = 0);
    void applyRelocation(const ResourceId& source, const ResourceId& destination, bool refreshView = true);
    bool canReplayExplorer(bool redo) const;
    void replayExplorer(bool redo);
    void recordExplorer(ExplorerEdit edit);
    void recordCreation(const ResourceId& path, const ResourceId& selection = {});
    void discardBackups(std::vector<ResourceId> paths);
    void stopExplorerHistory();
    bool canPasteImage(TreeItem item) const;
    void pasteImage(TreeItem item);
    wchar_t stateFor(const ResourceId& path) const;
    void openFile(const ResourceId& path, bool diff = false);
    void openContentResult(const ContentMatch& match, const std::string& query, bool matchCase);
    void applyContentResult(size_t index);
    void openWithDefaultApp(ResourceId path);
    void revealInFileExplorer(ResourceId path);
    void saveTab();
    std::shared_ptr<EditBuffer> shareBuffer(const ResourceId& path, std::shared_ptr<EditBuffer> loaded = {});
    bool saveFile(size_t index, bool wait = false);
    bool confirmTab(size_t index);
    void selectTab(size_t index);
    void closeTab(size_t index);
    void closeTabs(const std::vector<size_t>& indices);
    void toggleTabPin(size_t index);
    void tabMenu(POINT screen);
    void loadTab(bool preserve = false, int group = -1);
    void refreshEditorGit(int group);
    void updateTabs(bool reveal = true);
    bool tabVisible(size_t index) const;
    bool tabOverflow() const { return editorGroups_[focusedGroup_].overflow(); }
    RECT tabBounds(size_t index) const;
    size_t tabAt(POINT point) const;
    std::optional<TabDrop> tabDropAt(POINT point) const;
    void updateTabDrag(POINT point);
    void scrollTabDrag();
    void endTabDrag(bool drop = false);
    ResourceId selectedPath() const { return tabs_.empty() ? ResourceId{} : tabs_[activeTab_].path; }
    void showDiff();
    void refresh(bool force = true);
    void refreshGit(bool rescan = false);
    void watch();
    void armWatch();
    void pollWatch();
    void stopWatch();
    void status(std::wstring message);
    void updateStatusBar();
    void toggleTerminal();
    HWND window_ = nullptr, tree_ = nullptr, gitTree_ = nullptr, explorerButton_ = nullptr, gitButton_ = nullptr, searchButton_ = nullptr, tooltips_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Tree explorer_, changes_;
    Renderer renderer_;
    StatusBar statusBar_;
    bool statusDirty_ = true;
    size_t partialFolders_ = 0;
    std::optional<WatchUsage> watchUsage_;
    ActivityButton explorerActivity_, gitActivity_, searchActivity_;
    UINT dpi_ = 96;
    int sidebar_ = 245, terminalHeight_ = 260, footer_ = 26, splitter_ = 5;
    int activityWidth_ = 48, sidebarHeader_ = 34, gitHeader_ = 180;
    RECT sidebarRect_{}, sidebarSplitter_{}, terminalSplitter_{}, tabsRect_{};
    int dragging_ = 0, dragOffset_ = 0;
    bool terminalVisible_ = true;
    bool updatingGitFiles_ = false;
    bool renamePending_ = false;
    bool historyStopped_ = false;
    ExplorerHistory explorerHistory_;
    bool pastePending_ = false;
    bool createPending_ = false;
    bool gitStagePending_ = false;
    bool gitDiscardPending_ = false;
    bool gitStashPending_ = false;
    bool gitCommitPending_ = false;
    GitCommitPane gitCommit_;
    bool gitTreeView_ = true;
    uint64_t branchRequest_ = 0;
    bool branchesLoading_ = false, branchesLoaded_ = false, branchRepositories_ = false;
    std::wstring branchError_;
    std::set<std::wstring> collapsedGitPaths_;
    Sidebar activeSidebar_ = Sidebar::Explorer;
    Viewer viewers_[2];
    Viewer& viewer() { return viewers_[focusedGroup_]; }
    const Viewer& viewer() const { return viewers_[focusedGroup_]; }
    size_t groupTab(int group) const { for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].group == group && tabs_[i].displayed) return i; return tabs_.size(); }
    void normalizeGroups();
    int focusedGroup_ = 0, editorRatio_ = 500;
    EditorGroup editorGroups_[2];
    RECT editorRect_{}, editorSplitter_{};
    DropPreview dropPreview_;
    TerminalPane terminal_;
    QuickOpen quickOpen_;
    SearchPane searchPane_;
    std::unique_ptr<Worker> worker_, gitWorker_;
    ProjectRuntime* owner_ = nullptr;
    std::optional<ProjectSession> initialState_;
    HWND lastFocus_ = nullptr;
    std::string focusedPane_ = "viewer";
    std::string pendingGitSelection_;
    int restoreExplorerTop_ = 0, restoreExplorerLeft_ = 0, restoreGitTop_ = 0, restoreGitLeft_ = 0;
    bool restoredScroll_ = false;
    ResourceId root_;
    std::map<std::wstring, ResourceId> expandedPaths_;
    ResourceId pendingTreeSelection_;
    ResourceId pendingExplorerFile_;
    std::map<std::wstring, ResourceId> pendingTreeSelections_;
    bool waitingForGit_[2]{};
    std::vector<Tab> tabs_;
    std::vector<RECT> tabRects_;
    std::vector<size_t> visibleTabs_;
    size_t activeTab_ = 0, tabTools_ = 0;

    std::optional<TabDrag> tabDrag_;
    TreeItem rootItem_ = 0;
    uint64_t generation_ = 0, serial_[2]{}, editorGitSerial_[2]{}, nextId_ = 1;
    std::map<uint64_t, TreeItem> nodes_;
    std::shared_ptr<GitWorkspace> git_;
    bool gitScanDirty_ = true;
    uint64_t gitScanRevision_ = 0;
    std::shared_ptr<ProjectProvider> provider_;
    bool connecting_ = false, connected_ = false, active_ = false;
    bool watchLimited_ = false;
    void connectRemote();
    std::wstring status_ = L"Ready  ·  Ctrl+O: open folder";
    std::wstring tabTip_, connectionError_;
    std::string wslUid_;
    Handle watchDirectory_, watchEvent_;
    OVERLAPPED watchOperation_{};
    alignas(DWORD) std::array<BYTE, 64 * 1024> watchBuffer_{};
    bool watchPending_ = false, dirty_ = false, fileDirty_ = false, filesDirty_ = false;
    ULONGLONG dirtySince_ = 0, lastEvent_ = 0;
};
void ProjectRuntime::Impl::create() {
    worker_ = std::make_unique<Worker>(window_);
    gitWorker_ = std::make_unique<Worker>(window_);
    explorer_.create(window_, instance_, L"Files", 1); tree_ = explorer_.window();
    changes_.create(window_, instance_, L"Source Control", 4); gitTree_ = changes_.window();
    gitCommit_.create(window_, instance_);
    gitCommit_.requested = [this](const ResourceId& root, const std::wstring& message, bool amend) { commitGitChanges(root, message, amend); };
    explorer_.multipleSelection = true;
    explorer_.selectionChanged = [this] { pendingTreeSelection_.clear(); pendingTreeSelections_.clear(); pendingExplorerFile_.clear(); };
    explorer_.activated = [this](TreeItem item) {
        pendingTreeSelection_.clear();
        pendingTreeSelections_.clear();
        pendingExplorerFile_.clear();
        if (const auto n = node(item); n && !n->directory) openFile(n->path);
    };
    explorer_.doubleClicked = [this](TreeItem item) {
        const auto n = node(item); if (!n || n->directory || n->path.empty()) return;
        explorer_.activated(item);
        tabs_[activeTab_].preview = false; updateTabs();
        if (owner_ && owner_->changed) owner_->changed();
    };
    explorer_.expanded = [this](TreeItem item, bool value) {
        const auto n = node(item); if (!n) return;
        if (value) { expandedPaths_[pathKey(n->path)] = n->path; n->expandRequested = true; if (!n->loaded) requestDirectory(item); }
        else { expandedPaths_.erase(pathKey(n->path)); n->expandRequested = false; pendingExplorerFile_.clear(); }
    };
    explorer_.removed = [this](uintptr_t data) { const auto n = reinterpret_cast<Node*>(data); if (n) { nodes_.erase(n->id); if (n->truncated) --partialFolders_; statusDirty_ = true; delete n; } };
    explorer_.canCreate = [this](TreeItem item) { return canCreateItem(item); };
    explorer_.createCommitted = [this](TreeItem item, const std::wstring& name, bool directory) { createItem(item, name, directory); };
    explorer_.canRename = [this](TreeItem item) { return item != rootItem_ && node(item) && !node(item)->unsupported && !renamePending_ && !pastePending_ && !createPending_ && !gitDiscardPending_ && !gitStashPending_ && !gitCommitPending_ && (!root_.wsl() || (connected_ && provider_->state() == ConnectionState::Ready)); };
    explorer_.renameRequested = [this](TreeItem item) { if (const auto n = node(item); n && item != rootItem_ && !renamePending_) explorer_.rename(item, n->path.filename().native()); };
    explorer_.renameCommitted = [this](TreeItem item, const std::wstring& name) { renameFile(item, name); };
    explorer_.canDelete = [this](TreeItem item) { const auto n = node(item); return explorer_.canRename(item) && !gitStagePending_ && n && root_.contains(n->path) && pathKey(n->path) != pathKey(root_); };
    explorer_.deleteRequested = [this](const std::vector<TreeItem>& items) { deleteItems(items); };
    explorer_.canCopy = [this](TreeItem item) { return explorer_.canDelete(item); };
    explorer_.copyRequested = [this](const std::vector<TreeItem>& items, bool cut) { copyItems(items, cut); };
    explorer_.canPasteFiles = [this](TreeItem item) { return canPasteImage(item) && !gitStagePending_ && clipboardHasFiles(); };
    explorer_.pasteFilesRequested = [this](TreeItem item) { pasteFiles(item); };
    explorer_.canDrag = [this](TreeItem item) { return explorer_.canRename(item) && !gitStagePending_; };
    explorer_.canDrop = [this](TreeItem item, TreeItem parent) { return canMoveFile(item, parent); };
    explorer_.moveCommitted = [this](const std::vector<TreeItem>& items, TreeItem parent) { moveItems(items, parent); };
    explorer_.canOpenWithDefaultApp = [this](TreeItem item) { const auto n = node(item); return n && !n->directory && !n->unsupported && !root_.wsl(); };
    explorer_.openWithDefaultAppRequested = [this](TreeItem item) { if (const auto n = node(item); n && !n->directory) openWithDefaultApp(n->path); };
    explorer_.canRevealInFileExplorer = [this](TreeItem item) { const auto n = node(item); return n && !n->unsupported; };
    explorer_.revealInFileExplorerRequested = [this](TreeItem item) { if (const auto n = node(item)) revealInFileExplorer(n->path); };
    explorer_.canPasteImage = [this](TreeItem item) { return canPasteImage(item) && clipboardHasImage(); };
    explorer_.pasteImageRequested = [this](TreeItem item) { pasteImage(item); };
    explorer_.canUndo = [this] { return canReplayExplorer(false); };
    explorer_.canRedo = [this] { return canReplayExplorer(true); };
    explorer_.undoRequested = [this] { replayExplorer(false); };
    explorer_.redoRequested = [this] { replayExplorer(true); };
    changes_.removed = [](uintptr_t data) { delete reinterpret_cast<HistoryNode*>(data); };
    changes_.activated = [this](TreeItem item) { if (sourceNode(item)) openGitFile(item); else openHistory(item); };
    changes_.hoverActionsOnly = true;
    changes_.actionLabel = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (!source) {
            const auto n = historyNode(item);
            return !n ? L"" : n->kind == HistoryKind::Commit ? L"Open All Changes" : n->kind == HistoryKind::File ? L"Open File" : L"";
        }
        const auto& node = *source;
        if (node.group) return changes_.child(item) ? L"Open All Changes" : L"";
        return node.directory ? L"" : node.staged ? L"Unstage Changes" : L"Stage Changes";
    };
    changes_.actionIcon = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (!source) { const auto n = historyNode(item); return n && n->kind == HistoryKind::File ? Renderer::Icon::OpenFile : Renderer::Icon::OpenAllChanges; }
        if (source->group) return Renderer::Icon::OpenAllChanges;
        return source->staged ? Renderer::Icon::UnstageChanges : Renderer::Icon::StageChanges;
    };
    changes_.actionClicked = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (!source) {
            const auto n = historyNode(item);
            if (n && n->kind == HistoryKind::Commit) openGitDiff(n->root, GitDiffKind::Commit, n->revision, true, n->root);
            else if (n && n->kind == HistoryKind::File) openWorkingFile(n->path);
            return;
        }
        if (!git_) return;
        if (source->group) { const auto root = git_->repositories[source->repository]->root; openGitDiff(root, source->staged ? GitDiffKind::Staged : GitDiffKind::Changes, {}, true, root); }
        else stageGitChanges(item);
    };
    changes_.secondaryActionLabel = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (!source) return L"";
        const auto& node = *source;
        if (node.group) return changes_.child(item) ? node.staged ? L"Unstage All Changes" : L"Stage All Changes" : L"";
        return node.directory ? L"" : node.staged ? L"Open File" : L"Discard Changes";
    };
    changes_.secondaryActionIcon = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (source && !source->group) return source->staged ? Renderer::Icon::OpenFile : Renderer::Icon::DiscardChanges;
        return source && source->staged ? Renderer::Icon::UnstageChanges : Renderer::Icon::StageChanges;
    };
    changes_.secondaryActionClicked = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (!source) return;
        if (source->group) stageGitChanges(item, true);
        else if (source->staged) openGitFile(item, false);
        else discardGitChanges(item);
    };
    changes_.tertiaryActionLabel = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (!source) return L"";
        const auto& node = *source;
        if (node.group) return changes_.child(item) ? node.staged ? L"Stash Staged Changes" : L"Discard All Changes" : L"";
        return !node.directory && !node.staged ? L"Open File" : L"";
    };
    changes_.tertiaryActionIcon = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (source && !source->group) return Renderer::Icon::OpenFile;
        return source && source->staged ? Renderer::Icon::StashChanges : Renderer::Icon::DiscardChanges;
    };
    changes_.tertiaryActionClicked = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (!source) return;
        if (!source->group) openGitFile(item, false);
        else if (source->staged) stashGitChanges(item);
        else discardGitChanges(item, true);
    };
    changes_.quaternaryActionLabel = [this](TreeItem item) {
        const auto source = sourceNode(item);
        if (!source) return L"";
        const auto& node = *source;
        return node.group && !node.staged && changes_.child(item) ? L"Stash Unstaged Changes" : L"";
    };
    changes_.quaternaryActionIcon = [](TreeItem) { return Renderer::Icon::StashChanges; };
    changes_.quaternaryActionClicked = [this](TreeItem item) { stashGitChanges(item); };
    changes_.expanded = [this](TreeItem item, bool value) {
        const auto source = sourceNode(item);
        if (!source) {
            const auto n = historyNode(item);
            if (n && n->kind == HistoryKind::Repository) {
                const auto path = L"repository:" + pathKey(n->root) + L"\n";
                if (value) collapsedGitPaths_.erase(path);
                else if (collapsedGitPaths_.size() < maxTreeNodes) collapsedGitPaths_.insert(path);
            }
            if (value) requestHistory(item);
            return;
        }
        if (updatingGitFiles_) return;
        const auto& path = source->path;
        if (value) collapsedGitPaths_.erase(path);
        else if (collapsedGitPaths_.size() < maxTreeNodes) collapsedGitPaths_.insert(path);
    };
    explorerActivity_.create(window_, instance_, L"Explorer", explorerCommand, false); explorerButton_ = explorerActivity_.window();
    gitActivity_.create(window_, instance_, L"Source Control", gitCommand, true); gitButton_ = gitActivity_.window();
    searchActivity_.create(window_, instance_, L"Search", searchCommand, false, false, true); searchButton_ = searchActivity_.window();
    explorerActivity_.clicked = [this] { selectSidebar(Sidebar::Explorer); };
    gitActivity_.clicked = [this] { selectSidebar(Sidebar::Git); };
    searchActivity_.clicked = [this] { selectSidebar(Sidebar::Search); };
    explorerActivity_.select(true);
    statusBar_.create(window_, instance_);
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    for (const auto button : {explorerButton_, gitButton_, searchButton_}) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tip.hwnd = window_; tip.uId = reinterpret_cast<UINT_PTR>(button);
        tip.lpszText = const_cast<wchar_t*>(button == explorerButton_ ? L"Explorer (Ctrl+Shift+E)" : button == gitButton_ ? L"Source Control (Ctrl+Shift+G)" : L"Search (Ctrl+Shift+F)");
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    for (int group = 0; group < 2; ++group) {
        auto& view = viewers_[group]; view.create(window_, instance_);
        view.changed = [this, group] {
            saveTab();
            if (viewers_[1 - group].buffer() && viewers_[1 - group].buffer() == viewers_[group].buffer()) viewers_[1 - group].refreshBuffer();
            updateTabs(false); if (activeSidebar_ == Sidebar::Search) searchPane_.refresh(); if (owner_ && owner_->changed) owner_->changed();
        };
        view.save = [this, group] { saveFile(groupTab(group)); };
        view.diffLayoutChanged = [this, group] { viewers_[1 - group].setDiffLayout(viewers_[group].diffInline()); };
        view.statusChanged = [this] { statusDirty_ = true; };
        view.focused = [this, group] {
            const bool changed = focusedGroup_ != group;
            focusedGroup_ = group; if (groupTab(group) < tabs_.size()) activeTab_ = groupTab(group);
            if (changed) revealActiveFile();
            updateTabs(false);
        };
    }
    terminal_.create(window_, instance_);
    dropPreview_.create(window_, instance_);
    terminal_.changed = [this] { if (owner_ && owner_->changed) owner_->changed(); };
    quickOpen_.create(window_, instance_);
    quickOpen_.updated = [this] { statusDirty_ = true; };
    quickOpen_.opened = [this](const ResourceId& path) { openFile(path); SetFocus(viewer().window()); };
    searchPane_.create(window_, instance_);
    searchPane_.opened = [this](const ContentMatch& match, const std::string& query, bool matchCase) { openContentResult(match, query, matchCase); };
    searchPane_.exit = [this] { SetFocus(viewer().window()); };
    searchPane_.overlays = [this] {
        std::vector<SearchOverlay> result; std::set<std::string> seen;
        for (const auto& tab : tabs_) if (tab.buffer && tab.buffer->dirty() && root_.contains(tab.path) && seen.insert(tab.path.key()).second)
            result.push_back({tab.path.lexically_relative(root_).generic_u8string(), tab.buffer});
        return result;
    };
    // Each underlying pane must clip the palette when repainting beneath it.
    for (const auto pane : {tree_, gitTree_, searchPane_.window(), viewers_[0].window(), viewers_[1].window(), terminal_.container(), explorerButton_, gitButton_, searchButton_})
        SetWindowLongPtrW(pane, GWL_STYLE, GetWindowLongPtrW(pane, GWL_STYLE) | WS_CLIPSIBLINGS);
    const BOOL dark = TRUE; DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    dpi(GetDpiForWindow(window_));
}
void ProjectRuntime::Impl::dpi(UINT value) {
    dpi_ = value; renderer_.setDpi(dpi_);
    explorer_.setDpi(dpi_); changes_.setDpi(dpi_, 28);
    explorerActivity_.setDpi(dpi_); gitActivity_.setDpi(dpi_); searchActivity_.setDpi(dpi_); searchPane_.setDpi(dpi_);
    footer_ = MulDiv(26, dpi_, 96); splitter_ = MulDiv(5, dpi_, 96);
    activityWidth_ = MulDiv(48, dpi_, 96); sidebarHeader_ = MulDiv(34, dpi_, 96); gitHeader_ = MulDiv(180, dpi_, 96);
    gitCommit_.setDpi(dpi_);
    for (auto& view : viewers_) view.setDpi(dpi_);
    terminal_.setDpi(dpi_); quickOpen_.setDpi(dpi_); statusBar_.setDpi(dpi_); layout();
}
void ProjectRuntime::Impl::layout(bool revealTab) {
    if (!tree_) return;
    RECT r{}; GetClientRect(window_, &r);
    const int width = static_cast<int>(r.right), height = static_cast<int>(r.bottom);
    const int side = std::clamp(MulDiv(sidebar_, dpi_, 96), MulDiv(130, dpi_, 96), std::max(MulDiv(130, dpi_, 96), width - activityWidth_ - splitter_ - MulDiv(250, dpi_, 96)));
    const int right = activityWidth_ + side + splitter_, content = std::max(0, height - footer_);
    const int terminalSize = terminalVisible_ ? std::min(MulDiv(terminalHeight_, dpi_, 96), std::max(0, content - MulDiv(100, dpi_, 96))) : 0;
    const int viewerSize = std::max(0, content - terminalSize - (terminalVisible_ ? splitter_ : 0));
    sidebarRect_ = {activityWidth_, 0, activityWidth_ + side, content};
    sidebarSplitter_ = {sidebarRect_.right, 0, right, content};
    terminalSplitter_ = {right, viewerSize, width, viewerSize + (terminalVisible_ ? splitter_ : 0)};
    const int gitTop = sidebarHeader_ + gitHeader_;
    const bool showExplorer = activeSidebar_ == Sidebar::Explorer && !root_.empty();
    const bool showGitFiles = activeSidebar_ == Sidebar::Git && changes_.child();
    normalizeGroups();
    editorRect_ = {right, 0, width, viewerSize};
    const bool split = std::any_of(tabs_.begin(), tabs_.end(), [](const Tab& tab) { return tab.group == 1; });
    const int middle = right + MulDiv(std::max(0, width - right - splitter_), editorRatio_, 1000);
    editorSplitter_ = split ? RECT{middle, 0, middle + splitter_, viewerSize} : RECT{};
    const int rowHeight = MulDiv(34, dpi_, 96);
    const size_t maxRows = static_cast<size_t>(std::max(1, (viewerSize - MulDiv(100, dpi_, 96)) / rowHeight));
    tabRects_.assign(tabs_.size(), RECT{});
    for (int group = 0; group < 2; ++group) {
        auto& pane = editorGroups_[group]; pane.indices.clear();
        for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].group == group) pane.indices.push_back(i);
        const int start = group ? middle + splitter_ : right, edge = group || !split ? width : middle;
        pane.tabs = {start, 0, edge, 0};
        const auto wrap = [&](int available) {
            pane.starts.clear(); int x = start, row = -1;
            for (size_t j = 0; j < pane.indices.size(); ++j) {
                const size_t i = pane.indices[j]; const auto& tab = tabs_[i];
                const auto title = tabTitle(tab);
                // Reserve the modified marker even for clean tabs, so editing
                // does not change their width or hide the marker in the title.
                const int width = std::min(std::max(0, available), renderer_.textWidth(title, tab.preview) + MulDiv(52, dpi_, 96));
                if (!j || tab.pinned != tabs_[pane.indices[j - 1]].pinned || (x > start && x + width > start + available)) { pane.starts.push_back(j); x = start; ++row; }
                tabRects_[i] = {x, row * rowHeight, x + width, (row + 1) * rowHeight}; x += width;
            }
            pane.rows = std::min(maxRows, pane.starts.size());
        };
        wrap(edge - start); if (pane.overflow()) wrap(std::max(0, edge - start - MulDiv(56, dpi_, 96)));
        pane.firstRow = std::min(pane.firstRow, pane.starts.size() - pane.rows);
        const auto selected = groupTab(group);
        const auto found = std::find(pane.indices.begin(), pane.indices.end(), selected);
        if (revealTab && found != pane.indices.end()) {
            const auto row = static_cast<size_t>(std::upper_bound(pane.starts.begin(), pane.starts.end(), static_cast<size_t>(found - pane.indices.begin())) - pane.starts.begin() - 1);
            if (row < pane.firstRow) pane.firstRow = row;
            else if (row >= pane.firstRow + pane.rows) pane.firstRow = row - pane.rows + 1;
        }
        pane.tabs.bottom = static_cast<int>(pane.rows) * rowHeight;
        pane.body = {start, pane.tabs.bottom, edge, viewerSize};
        for (const auto i : pane.indices) OffsetRect(&tabRects_[i], 0, -static_cast<int>(pane.firstRow) * rowHeight);
    }
    tabsRect_ = {right, 0, width, std::max(editorGroups_[0].tabs.bottom, editorGroups_[1].tabs.bottom)};
    updateTabs(false);
    if (tabDrag_ && tabDrag_->active) tabDrag_->target = tabDropAt(tabDrag_->point);

    struct Placement { HWND window; int x, y, width, height, flags; };
    const std::array<Placement, 11> panes{{
        {statusBar_.window(), 0, std::max(0, height - footer_), width, footer_, SWP_SHOWWINDOW},
        {explorerButton_, 0, 0, activityWidth_ - 1, activityWidth_, SWP_SHOWWINDOW},
        {searchButton_, 0, activityWidth_, activityWidth_ - 1, activityWidth_, SWP_SHOWWINDOW},
        {gitButton_, 0, 2 * activityWidth_, activityWidth_ - 1, activityWidth_, SWP_SHOWWINDOW},
        {tree_, activityWidth_, sidebarHeader_, side, std::max(0, content - sidebarHeader_), showExplorer ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {gitTree_, activityWidth_, gitTop, side, std::max(0, content - gitTop), showGitFiles ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {gitCommit_.window(), activityWidth_ + MulDiv(12, dpi_, 96), sidebarHeader_ + MulDiv(56, dpi_, 96), side - MulDiv(24, dpi_, 96), MulDiv(90, dpi_, 96), activeSidebar_ == Sidebar::Git ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {searchPane_.window(), activityWidth_, sidebarHeader_, side, std::max(0, content - sidebarHeader_), activeSidebar_ == Sidebar::Search ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {viewers_[0].window(), editorGroups_[0].body.left, editorGroups_[0].body.top, std::max(0L, editorGroups_[0].body.right - editorGroups_[0].body.left), std::max(0L, editorGroups_[0].body.bottom - editorGroups_[0].body.top), SWP_SHOWWINDOW},
        {viewers_[1].window(), editorGroups_[1].body.left, editorGroups_[1].body.top, std::max(0L, editorGroups_[1].body.right - editorGroups_[1].body.left), std::max(0L, editorGroups_[1].body.bottom - editorGroups_[1].body.top), split ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {terminal_.container(), right, viewerSize + splitter_, std::max(0, width - right), terminalSize,
            terminalVisible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE}
    }};
    // Move every pane before painting. Copying old window pixels during a
    // Direct2D resize can carry the previous splitter into the new layout.
    constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
    HDWP positions = BeginDeferWindowPos(static_cast<int>(panes.size()));
    for (const auto& pane : panes) {
        if (!positions) break;
        positions = DeferWindowPos(positions, pane.window, nullptr, pane.x, pane.y, pane.width, pane.height, flags | pane.flags);
    }
    if (!positions || !EndDeferWindowPos(positions)) {
        for (const auto& pane : panes)
            SetWindowPos(pane.window, nullptr, pane.x, pane.y, pane.width, pane.height, flags | pane.flags);
    }
    // WM_PAINT can be delayed by a stream of mouse moves. Present the parent
    // and all visible panes now, including the area the old splitters occupied.
    terminal_.acknowledge();
    quickOpen_.layout();
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
void ProjectRuntime::Impl::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    auto& dc = renderer_;
    RECT r{}; GetClientRect(window_, &r);
    dc.fill(r, border); dc.fill({0, r.bottom - footer_, r.right, r.bottom}, panel);
    auto scaled = [&](int v) { return MulDiv(v, dpi_, 96); };
    dc.fill({0, 0, activityWidth_, r.bottom - footer_}, background);
    dc.fill({activityWidth_ - 1, 0, activityWidth_, r.bottom - footer_}, border);
    dc.fill(sidebarRect_, panel);
    const int left = sidebarRect_.left + scaled(14), edge = sidebarRect_.right - scaled(12), top = sidebarHeader_;
    dc.label({left, 0, edge, top}, activeSidebar_ == Sidebar::Explorer ? L"EXPLORER" : activeSidebar_ == Sidebar::Git ? L"SOURCE CONTROL" : L"SEARCH", foreground);
    dc.fill({sidebarRect_.left, top - 1, sidebarRect_.right, top}, border);
    if (activeSidebar_ == Sidebar::Git) {
        const bool repository = git_ && !git_->repositories.empty();
        dc.label({left, top, edge, top + scaled(28)}, repository ? (git_->repositories.size() == 1 ? git_->repositories.front()->branch : std::to_wstring(git_->repositories.size()) + L" repositories") : L"Git", repository ? accent : muted);
        const auto summary = repository ? std::to_wstring(git_->changed + git_->untracked) + L" pending changes" : L"";
        dc.label({left, top + scaled(28), edge, top + scaled(56)}, summary, muted);
        for (const bool tree : {true, false}) {
            const auto button = gitViewButton(tree);
            dc.fill(button, tree == gitTreeView_ ? rgb(50, 70, 94) : background);
            dc.label(button, tree ? L"Tree" : L"List", tree == gitTreeView_ ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        dc.fill({sidebarRect_.left, top + gitHeader_ - 1, sidebarRect_.right, top + gitHeader_}, border);
        if (!changes_.child()) {
            const std::wstring message = root_.empty() ? L"Open a folder to view source control." : !git_ || branchesLoading_ ? L"Loading source control…" : !git_->error.empty() ? L"Git unavailable.\n" + git_->error.substr(0, 2048) + L"\nPress F5 to retry." : git_->repositories.empty() ? L"No Git repositories found in this folder." : !branchError_.empty() ? branchError_ + L"\nPress F5 to retry." : L"No branches or commits yet.";
            dc.label({left, top + gitHeader_ + scaled(16), edge, sidebarRect_.bottom - scaled(12)}, message, muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    } else if (root_.empty()) {
        dc.label({left, top + scaled(16), edge, sidebarRect_.bottom - scaled(12)}, L"Open a folder with Ctrl+O to browse files.", muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    if (!tabs_.empty()) {
        dc.fill(tabsRect_, background);
        for (const auto i : visibleTabs_) {
            const auto rect = tabBounds(i);
            const bool active = tabs_[i].displayed;
            dc.fill(rect, active ? panel : background);
            if (active) dc.fill({rect.left, rect.top, rect.right, rect.top + scaled(2)}, accent);
            const auto title = tabTitle(tabs_[i]);
            dc.label({rect.left + scaled(12), rect.top, rect.right - scaled(40), rect.bottom}, title, active ? foreground : muted,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, tabs_[i].preview);
            // Draw unsaved state outside the ellipsized filename, before the
            // close/pin control. This also applies to editable diff tabs.
            if (tabs_[i].buffer && tabs_[i].buffer->dirty())
                dc.label({rect.right - scaled(40), rect.top, rect.right - scaled(28), rect.bottom}, L"*", active ? foreground : muted,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (tabs_[i].pinned) {
                const float x = static_cast<float>(rect.right - scaled(16)), y = static_cast<float>((rect.top + rect.bottom) / 2);
                const float unit = static_cast<float>(dpi_) / 96.0f;
                const auto pinLine = [&](int x1, int y1, int x2, int y2) {
                    dc.line(x + x1 * unit, y + y1 * unit, x + x2 * unit, y + y2 * unit, accent, unit);
                };
                pinLine(-4, -7, 4, -7); pinLine(-3, -7, -3, -2); pinLine(3, -7, 3, -2);
                pinLine(-3, -2, -6, 2); pinLine(3, -2, 6, 2); pinLine(-6, 2, 6, 2); pinLine(0, 2, 0, 8);
            } else dc.label({rect.right - scaled(28), rect.top, rect.right - scaled(4), rect.bottom}, L"×", muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            dc.fill({rect.right - 1, rect.top, rect.right, rect.bottom}, border);
        }
        for (const auto& pane : editorGroups_) {
            if (pane.overflow()) {
                const int edge = pane.tabs.right, button = scaled(28), top = pane.tabs.bottom - scaled(34);
                dc.label({edge - 2 * button, top, edge - button, pane.tabs.bottom}, L"‹", pane.firstRow ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                dc.label({edge - button, top, edge, pane.tabs.bottom}, L"›", pane.firstRow + pane.rows < pane.starts.size() ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            for (size_t row = 1; row <= pane.rows; ++row) { const int bottom = pane.tabs.top + static_cast<int>(row) * scaled(34); dc.fill({pane.tabs.left, bottom - 1, pane.tabs.right, bottom}, border); }
        }
        if (tabDrag_ && tabDrag_->active && tabDrag_->target) dc.fill(tabDrag_->target->marker, accent);
    }
}
void ProjectRuntime::Impl::selectSidebar(Sidebar sidebar) {
    activeSidebar_ = sidebar; layout();
    explorerActivity_.select(sidebar == Sidebar::Explorer); gitActivity_.select(sidebar == Sidebar::Git);
    searchActivity_.select(sidebar == Sidebar::Search); searchPane_.active(sidebar == Sidebar::Search && active_);
    if (sidebar == Sidebar::Search) { searchPane_.focus(); return; }
    if (sidebar == Sidebar::Git) refreshGit();
    if (sidebar == Sidebar::Explorer) explorer_.showSelection();
    SetFocus(sidebar == Sidebar::Explorer ? (IsWindowVisible(tree_) ? tree_ : explorerButton_) : (IsWindowVisible(gitTree_) ? gitTree_ : gitButton_));
}
RECT ProjectRuntime::Impl::gitViewButton(bool tree) const {
    const int padding = MulDiv(12, dpi_, 96), top = sidebarHeader_;
    const int middle = (sidebarRect_.left + sidebarRect_.right) / 2;
    return {tree ? sidebarRect_.left + padding : middle, top + gitHeader_ - MulDiv(25, dpi_, 96),
        tree ? middle : sidebarRect_.right - padding, top + gitHeader_ - MulDiv(3, dpi_, 96)};
}
void ProjectRuntime::Impl::setGitView(bool tree) {
    if (gitTreeView_ == tree) return;
    gitTreeView_ = tree;
    const int top = changes_.top(), left = changes_.left();
    {
        Tree::Batch batch(changes_);
        std::vector<TreeItem> commits, pending;
        for (auto item = changes_.child(); item; item = changes_.next(item)) pending.push_back(item);
        while (!pending.empty()) {
            const auto item = pending.back(); pending.pop_back(); const auto n = historyNode(item);
            if (n->kind == HistoryKind::Commit) { if (n->loaded) commits.push_back(item); }
            else if (n->kind == HistoryKind::Branch || n->kind == HistoryKind::Repository)
                for (auto child = changes_.child(item); child; child = changes_.next(child)) pending.push_back(child);
        }
        for (const auto item : commits) updateCommitFiles(item);
        updateGitFiles();
    }
    changes_.restoreScroll(top, left);
    saveTab();
    for (int group = 0; group < 2; ++group) {
        const auto index = groupTab(group);
        if (index < tabs_.size() && tabs_[index].diff && tabs_[index].allChanges) loadTab(true, group);
    }
    if (IsWindowVisible(gitTree_)) SetFocus(gitTree_);
}
std::wstring ProjectRuntime::Impl::repositoryLabel(const ResourceId& root) const {
    if (root_.contains(root) && pathKey(root_) != pathKey(root)) return root.lexically_relative(root_).generic_wstring();
    return root.filename().empty() ? root.native() : root.filename().native();
}
SourceNode* ProjectRuntime::Impl::sourceNode(TreeItem item) const {
    const auto n = historyNode(item); return n && n->kind == HistoryKind::Source ? &n->source : nullptr;
}
void ProjectRuntime::Impl::clearHistoryChildren(TreeItem item) {
    // Working changes belong to the branch, independently of history loading.
    for (auto child = changes_.child(item); child;) {
        const auto next = changes_.next(child);
        if (!sourceNode(child)) changes_.erase(child);
        child = next;
    }
}
void ProjectRuntime::Impl::sortHistory(TreeItem item) {
    changes_.sort(item, [](uintptr_t a, uintptr_t b) {
        const auto rank = [](const HistoryNode* node) { return node->kind == HistoryKind::Source ? (node->source.staged ? 0 : 1) : 2; };
        return rank(reinterpret_cast<HistoryNode*>(a)) < rank(reinterpret_cast<HistoryNode*>(b));
    });
}
void ProjectRuntime::Impl::updateGitFiles() {
    gitActivity_.setBadgeCount(git_ ? git_->changed + git_->untracked : 0);
    std::wstring selected = wide(pendingGitSelection_);
    const int oldTop = changes_.top(), oldLeft = changes_.left();
    if (const auto source = sourceNode(changes_.selection())) selected = source->path;
    updatingGitFiles_ = true;
    {
        Tree::Batch batch(changes_);
        std::vector<TreeItem> branches, pending;
        for (auto item = changes_.child(); item; item = changes_.next(item)) pending.push_back(item);
        while (!pending.empty()) {
            const auto item = pending.back(); pending.pop_back(); const auto n = historyNode(item);
            if (n->kind == HistoryKind::Branch) branches.push_back(item);
            else if (n->kind == HistoryKind::Repository)
                for (auto child = changes_.child(item); child; child = changes_.next(child)) pending.push_back(child);
        }
        for (const auto branch : branches) for (auto child = changes_.child(branch); child;) {
            const auto next = changes_.next(child);
            if (sourceNode(child)) changes_.erase(child);
            child = next;
        }
        TreeItem selection = changes_.selection();
        if (git_) for (const auto branch : branches) {
            const auto history = historyNode(branch); if (!history->current) continue;
            const auto found = std::find_if(git_->repositories.begin(), git_->repositories.end(), [&](const auto& repo) { return pathKey(repo->root) == pathKey(history->root); });
            if (found == git_->repositories.end() || !(*found)->error.empty()) continue;
            const auto repository = static_cast<size_t>(found - git_->repositories.begin()); const auto& repo = **found;
            const bool multiple = git_->repositories.size() > 1;
            const auto repoKey = multiple ? L"repository:" + pathKey(repo.root) + L"\n" : std::wstring{};
            const auto insertSource = [&](TreeItem parent, std::wstring text, SourceNode source, bool expandable, COLORREF color = foreground) {
                HistoryNode node{HistoryKind::Source, {}, {}, {}}; node.source = std::move(source);
                return insertHistory(parent, std::move(text), std::move(node), expandable, color);
            };
            for (const bool staged : {true, false}) {
                const std::wstring prefix = repoKey + (staged ? L"staged:" : L"changes:");
                std::vector<std::wstring> paths; std::vector<size_t> entries;
                for (size_t i = 0; i < repo.entries.size(); ++i) {
                    const auto& entry = repo.entries[i];
                    if (staged ? !entry.staged : !entry.changed) continue;
                    paths.push_back(entry.path.lexically_relative(repo.root).generic_wstring()); entries.push_back(i);
                }
                const auto group = insertSource(branch, (staged ? L"Staged Changes" : L"Changes") + (L" (" + std::to_wstring(entries.size()) + L")"),
                    {prefix, noGitChange, true, staged, true, repository}, true);
                const auto nodes = buildGitChanges(paths, gitTreeView_);
                std::vector<TreeItem> items; items.reserve(nodes.size());
                for (const auto& node : nodes) {
                    const auto parent = node.parent == noGitChange ? group : items[node.parent];
                    const auto offset = node.parent == noGitChange ? 0 : nodes[node.parent].path.size() + 1;
                    const auto entryIndex = node.directory() ? noGitChange : entries[node.entry];
                    const auto state = node.directory() ? wchar_t{} : staged ? static_cast<wchar_t>(repo.entries[entryIndex].index) : repo.entries[entryIndex].state;
                    const auto item = insertSource(parent, node.path.substr(offset), {prefix + node.path, entryIndex, node.directory(), staged, false, repository},
                        node.directory(), node.directory() ? foreground : stateColor(state));
                    items.push_back(item);
                    if (node.directory()) changes_.expand(item, !collapsedGitPaths_.count(prefix + node.path));
                    if (prefix + node.path == selected || (!multiple && !staged && node.path == selected)) selection = item;
                }
                changes_.expand(group, !collapsedGitPaths_.count(prefix));
                if (selected == prefix) selection = group;
            }
            sortHistory(branch);
        }
        for (auto item = selection; item;) {
            const auto node = changes_.item(item); item = node ? node->parent : 0;
            if (const auto parent = changes_.item(item); parent && !parent->expanded) selection = item;
        }
        changes_.select(selection, false);
    }
    updatingGitFiles_ = false;
    if (git_ && branchesLoaded_) { changes_.restoreScroll(restoreGitTop_ ? restoreGitTop_ : oldTop, restoreGitLeft_ ? restoreGitLeft_ : oldLeft); restoreGitTop_ = restoreGitLeft_ = 0; pendingGitSelection_.clear(); }
    layout(false);
}
void ProjectRuntime::Impl::openGitFile(TreeItem item, bool diff) {
    if (updatingGitFiles_ || !git_) return;
    const auto source = sourceNode(item);
    if (!source) return;
    const auto& node = *source;
    const auto& repo = *git_->repositories[node.repository];
    if (node.group || node.directory || node.entry >= repo.entries.size()) return;
    const auto entry = repo.entries[node.entry];
    if (!diff) {
        openWorkingFile(entry.path);
        return;
    }
    if (entry.state == L'?') { openFile(entry.path); return; }
    openGitDiff(entry.path, node.staged ? GitDiffKind::Staged : GitDiffKind::Changes, {}, false, repo.root);
}
void ProjectRuntime::Impl::openWorkingFile(const ResourceId& path) {
    openFile(path);
    // A normal file tab may already have been switched to diff with F6.
    if (activeTab_ < tabs_.size() && tabs_[activeTab_].diff) {
        saveTab(); tabs_[activeTab_].diff = false; loadTab();
    }
    SetFocus(viewer().window());
}
void ProjectRuntime::Impl::stageGitChanges(TreeItem item, bool all) {
    if (updatingGitFiles_ || gitStagePending_ || gitDiscardPending_ || gitStashPending_ || gitCommitPending_ || renamePending_ || !git_ || git_->repositories.empty()) return;
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { status(L"Reconnect WSL before staging changes."); return; }
    const auto source = sourceNode(item);
    if (!source) return;
    const auto& node = *source;
    if (node.repository >= git_->repositories.size()) return;
    const auto& repo = *git_->repositories[node.repository];
    if (!repo.error.empty()) return;
    if (all) { if (!node.group || !changes_.child(item)) return; }
    else if (node.directory || node.entry >= repo.entries.size()) return;
    const bool stage = !node.staged;
    std::vector<std::string> arguments;
    std::wstring name;
    if (all) { arguments = stage ? gitStageAllArguments() : gitUnstageAllArguments(); name = L"all changes in " + repositoryLabel(repo.root); }
    else {
        const auto& entry = repo.entries[node.entry];
        arguments = gitStageArguments({entry.path.lexically_relative(repo.root).generic_u8string(), entry.previousPath, entry.index, entry.worktree}, stage);
        name = entry.path.filename().native();
    }
    const auto args = gitArguments(arguments);
    const auto root = repo.root; const auto provider = provider_; const auto generation = generation_;
    gitStagePending_ = true;
    status((stage ? L"Staging " : L"Unstaging ") + name + L"…");
    gitWorker_->submit(L"git-stage", [this, args, root, provider, generation, name, stage] {
        const auto result = provider->git(args, root, gitWorker_->cancellation());
        gitWorker_->complete([this, generation, name, stage, code = result.code, error = result.error] {
            gitStagePending_ = false;
            if (generation != generation_) return;
            if (code) { refreshGit(); status((stage ? L"Stage failed: " : L"Unstage failed: ") + error); return; }
            refresh(false);
            status((stage ? L"Staged " : L"Unstaged ") + name);
        });
    });
}
void ProjectRuntime::Impl::discardGitChanges(TreeItem item, bool all) {
    if (updatingGitFiles_ || gitStagePending_ || gitDiscardPending_ || gitStashPending_ || gitCommitPending_ || renamePending_ || pastePending_ || createPending_ || !git_) return;
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { status(L"Reconnect WSL before discarding changes."); return; }
    const auto source = sourceNode(item);
    if (!source) return;
    const auto& node = *source;
    if (node.staged || node.repository >= git_->repositories.size()) return;
    const auto& repo = *git_->repositories[node.repository];
    if (all) { if (!node.group || !changes_.child(item)) return; }
    else if (node.group || node.directory || node.entry >= repo.entries.size()) return;
    if (!repo.error.empty()) return;
    if (std::any_of(tabs_.begin(), tabs_.end(), [](const Tab& tab) { return tab.buffer && tab.buffer->saving; })) {
        status(L"Wait for file saves to finish before discarding changes."); return;
    }
    // MessageBox dispatches UI completions. Copy the target before entering it.
    const auto root = repo.root;
    const auto path = all ? std::string{} : repo.entries[node.entry].path.lexically_relative(root).generic_u8string();
    const auto name = all ? repositoryLabel(root) : wide(path);
    const auto provider = provider_; const auto generation = generation_;
    gitDiscardPending_ = true;
    const auto message = all ? L"Discard all unstaged changes in " + root.native() +
        L"?\n\nWorking files will be restored from the index. Untracked files and folders will be permanently deleted. This cannot be undone.\n\n"
        L"Staged changes, ignored files, untracked nested repositories, submodules and unsaved editor text will be kept." :
        L"Discard unstaged changes to " + name + L" in " + root.native() +
        L"?\n\nThis file will be restored from the index, or permanently deleted if it is untracked. This cannot be undone.\n\n"
        L"Staged changes, other files and unsaved editor text will be kept.";
    const auto answer = MessageBoxW(window_, message.c_str(),
        all ? L"Poppy — Discard All Changes" : L"Poppy — Discard Changes", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDOK || generation != generation_) { gitDiscardPending_ = false; return; }
    status(L"Discarding changes in " + name + L"…");
    gitWorker_->submit(L"git-discard", [this, root, provider, generation, name, all, path] {
        // A removed .git must not redirect this destructive operation to a parent.
        auto result = provider->git({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--show-toplevel"}, root, gitWorker_->cancellation());
        if (!result.code && (pathKey(root.at(utf8(trimmed(result.output)))) != pathKey(root))) {
            result.code = 1; result.error = L"The Git repository changed. Refresh before discarding changes.";
        }
        // Read complete status here, rather than using the bounded UI snapshot.
        if (!result.code) result = provider->git(gitArguments(gitStatusArguments()), root, gitWorker_->cancellation());
        if (!result.code) try {
            const auto entries = parseGitStatus(result.output);
            const auto commands = all ? gitDiscardAllArguments(entries) : gitDiscardFileArguments(entries, path);
            for (const auto& command : commands) {
                result = provider->git(gitArguments(command), root, gitWorker_->cancellation());
                if (result.code) break;
            }
        } catch (const std::exception& error) { result.code = 1; result.error = wide(error.what()); }
        gitWorker_->complete([this, generation, name, code = result.code, error = result.error] {
            gitDiscardPending_ = false;
            if (generation != generation_) return;
            filesDirty_ = fileDirty_ = true;
            refresh(false);
            if (activeSidebar_ == Sidebar::Search) searchPane_.refresh();
            status(code ? L"Discard failed: " + error : L"Discard completed in " + name);
        });
    });
}
void ProjectRuntime::Impl::stashGitChanges(TreeItem item) {
    if (updatingGitFiles_ || gitStagePending_ || gitDiscardPending_ || gitStashPending_ || gitCommitPending_ || renamePending_ || pastePending_ || createPending_ || !git_) return;
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { status(L"Reconnect WSL before stashing changes."); return; }
    const auto source = sourceNode(item);
    if (!source) return;
    const auto& node = *source;
    if (!node.group || !changes_.child(item) || node.repository >= git_->repositories.size()) return;
    const auto& repo = *git_->repositories[node.repository];
    if (!repo.error.empty()) return;
    if (std::any_of(tabs_.begin(), tabs_.end(), [](const Tab& tab) { return tab.buffer && tab.buffer->saving; })) {
        status(L"Wait for file saves to finish before stashing changes."); return;
    }
    const auto root = repo.root; const auto name = repositoryLabel(root);
    const auto provider = provider_; const auto generation = generation_;
    gitStashPending_ = true;
    const bool staged = node.staged;
    status((staged ? L"Stashing staged changes in " : L"Stashing unstaged changes in ") + name + L"…");
    gitWorker_->submit(staged ? L"git-stash-staged" : L"git-stash-unstaged", [this, root, provider, generation, staged] {
        auto result = provider->git({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--show-toplevel"}, root, gitWorker_->cancellation());
        if (!result.code && pathKey(root.at(utf8(trimmed(result.output)))) != pathKey(root)) {
            result.code = 1; result.error = L"The Git repository changed. Refresh before stashing changes.";
        }
        if (!result.code) result = provider->git(gitArguments(gitStatusArguments()), root, gitWorker_->cancellation());
        if (!result.code) try {
            const auto entries = parseGitStatus(result.output);
            const auto arguments = staged ? gitStashStagedArguments(entries) : gitStashUnstagedArguments(entries);
            if (arguments.empty()) result.output = staged ? "No staged changes to stash." : "No unstaged changes to stash.";
            else result = provider->git(gitArguments(arguments), root, gitWorker_->cancellation());
        } catch (const std::exception& error) { result.code = 1; result.error = wide(error.what()); }
        gitWorker_->complete([this, generation, code = result.code, error = result.error, output = result.output] {
            gitStashPending_ = false;
            if (generation != generation_) return;
            filesDirty_ = fileDirty_ = true;
            refresh(false);
            if (activeSidebar_ == Sidebar::Search) searchPane_.refresh();
            status(code ? L"Stash failed: " + error : L"Stash: " + trimmed(output));
        });
    });
}
void ProjectRuntime::Impl::commitGitChanges(const ResourceId& root, std::wstring message, bool amend) {
    if (gitCommitPending_ || gitStagePending_ || gitDiscardPending_ || gitStashPending_ || renamePending_ || pastePending_ || createPending_) {
        status(L"Wait for the current operation to finish before committing."); return;
    }
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { status(L"Reconnect WSL before committing."); return; }
    if (!git_ || std::none_of(git_->repositories.begin(), git_->repositories.end(), [&](const auto& repo) { return repo->root == root && repo->error.empty(); })) return;
    if (std::any_of(tabs_.begin(), tabs_.end(), [](const Tab& tab) { return tab.buffer && tab.buffer->saving; })) {
        status(L"Wait for file saves to finish before committing."); return;
    }
    const auto provider = provider_; const auto generation = generation_;
    gitCommitPending_ = true; gitCommit_.busy(true);
    status((amend ? L"Amending the last commit in " : L"Committing staged changes in ") + repositoryLabel(root) + L"…");
    gitWorker_->submit(L"git-commit", [this, root, provider, generation, message = std::move(message), amend] {
        // Do not let a removed .git redirect a commit into an ancestor repository.
        auto result = provider->git({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--show-toplevel"}, root, gitWorker_->cancellation());
        if (!result.code && pathKey(root.at(utf8(trimmed(result.output)))) != pathKey(root)) {
            result.code = 1; result.error = L"The Git repository changed. Refresh before committing.";
        }
        if (!result.code) result = provider->git(gitArguments(gitStatusArguments()), root, gitWorker_->cancellation());
        if (!result.code) try {
            const auto arguments = gitCommitArguments(parseGitStatus(result.output), utf8(message), amend);
            result = provider->git(gitArguments(arguments), root, gitWorker_->cancellation());
        } catch (const std::exception& error) { result.code = 1; result.error = wide(error.what()); }
        gitWorker_->complete([this, root, generation, message, amend, code = result.code, error = result.error, output = result.output] {
            gitCommitPending_ = false; gitCommit_.busy(false);
            if (generation != generation_) return;
            if (!code) gitCommit_.committed(root, message);
            refresh(false);
            status(code ? L"Commit failed: " + (error.empty() ? trimmed(output) : error) :
                (amend ? L"Amended the last commit in " : L"Committed staged changes in ") + repositoryLabel(root));
        });
    });
}
HistoryNode* ProjectRuntime::Impl::historyNode(TreeItem item) const {
    const auto value = changes_.item(item); return value ? reinterpret_cast<HistoryNode*>(value->data) : nullptr;
}
TreeItem ProjectRuntime::Impl::insertHistory(TreeItem parent, std::wstring text, HistoryNode node, bool expandable, COLORREF color) {
    if (node.root.empty()) if (const auto n = historyNode(parent)) node.root = n->root;
    auto data = std::make_unique<HistoryNode>(std::move(node));
    const auto item = changes_.insert(parent, std::move(text), expandable, reinterpret_cast<uintptr_t>(data.get()), color);
    if (item) data.release();
    return item;
}
void ProjectRuntime::Impl::refreshBranches() {
    if (!git_ || git_->repositories.empty() || (root_.wsl() && !connected_)) return;
    const auto workspace = git_; const auto generation = generation_, request = ++branchRequest_;
    branchesLoading_ = true;
    gitWorker_->submit(L"branches", [this, workspace, generation, request] {
        struct Listing { ResourceId root; std::wstring branch, error; std::vector<GitBranch> refs; };
        std::vector<Listing> listings;
        for (const auto& repo : workspace->repositories) {
            if (*gitWorker_->cancellation()) return;
            Listing listing{repo->root, repo->branch, repo->error, {}};
            if (listing.error.empty()) {
                auto result = provider_->git({L"git.exe", L"--no-optional-locks", L"for-each-ref", L"--sort=refname",
                    L"--format=%(refname)%00%(refname:short)%00%(objectname)%00%(HEAD)", L"refs/heads", L"refs/remotes"}, repo->root, gitWorker_->cancellation());
                listing.refs = parseGitBranches(result.output);
                if (result.code) listing.error = result.error;
                // Detached HEAD remains browsable even without a branch.
                if (!result.code && std::none_of(listing.refs.begin(), listing.refs.end(), [](const GitBranch& ref) { return ref.current; })) {
                    auto head = provider_->git({L"git.exe", L"rev-parse", L"--verify", L"HEAD"}, repo->root, gitWorker_->cancellation());
                    if (!head.code) listing.refs.insert(listing.refs.begin(), {"HEAD", "HEAD (detached)", utf8(trimmed(head.output)), true});
                    else {
                        auto symbolic = provider_->git({L"git.exe", L"--no-optional-locks", L"symbolic-ref", L"-q", L"HEAD"}, repo->root, gitWorker_->cancellation());
                        if (!symbolic.code) listing.refs.insert(listing.refs.begin(), {utf8(trimmed(symbolic.output)), utf8(listing.branch), {}, true});
                    }
                }
            }
            listings.push_back(std::move(listing));
        }
        gitWorker_->complete([this, generation, request, listings = std::move(listings)] {
            if (generation != generation_ || request != branchRequest_) return;
            branchesLoading_ = false; branchError_.clear();
            const bool multiple = listings.size() > 1;
            bool sourceChanged = !branchesLoaded_ || multiple != branchRepositories_;
            Tree::Batch batch(changes_);
            if (multiple != branchRepositories_) { changes_.clear(); branchesLoaded_ = false; branchRepositories_ = multiple; }
            std::map<std::string, TreeItem> repositories;
            if (multiple) for (auto item = changes_.child(); item; item = changes_.next(item)) if (const auto n = historyNode(item)) repositories[n->root.key()] = item;
            for (const auto& listing : listings) {
                TreeItem parent = 0; bool fresh = !branchesLoaded_;
                if (multiple) {
                    const auto found = repositories.find(listing.root.key());
                    if (found != repositories.end()) { parent = found->second; repositories.erase(found); }
                    else {
                        HistoryNode node{HistoryKind::Repository, {}, {}, {}}; node.root = listing.root;
                        parent = insertHistory(0, L"", std::move(node), true, accent); fresh = true;
                    }
                    changes_.label(parent, repositoryLabel(listing.root) + L" · " + listing.branch, accent);
                }
                std::map<std::string, TreeItem> existing;
                for (auto item = changes_.child(parent); item;) {
                    const auto next = changes_.next(item); const auto n = historyNode(item);
                    if (n && n->kind == HistoryKind::Branch && pathKey(n->root) == pathKey(listing.root)) existing[n->ref] = item;
                    else changes_.erase(item);
                    item = next;
                }
                if (!listing.error.empty()) {
                    // Keep an error local to its repository; siblings remain usable.
                    for (const auto& stale : existing) changes_.erase(stale.second);
                    if (multiple) insertHistory(parent, L"Git unavailable: " + listing.error, {HistoryKind::Message, {}, {}, {}}, false, muted);
                    else branchError_ = listing.error;
                } else {
                    for (const auto& ref : listing.refs) {
                        const auto found = existing.find(ref.ref);
                        const auto text = (ref.current ? L"● " : L"") + wide(ref.name);
                        if (found == existing.end()) {
                            HistoryNode node{HistoryKind::Branch, ref.ref, ref.tip, {}}; node.root = listing.root; node.current = ref.current;
                            sourceChanged = sourceChanged || ref.current;
                            const auto item = insertHistory(parent, text, std::move(node), true, ref.current ? accent : foreground);
                            if (ref.current) changes_.expand(item, true);
                        } else {
                            const auto item = found->second; auto n = historyNode(item);
                            changes_.label(item, text, ref.current ? accent : foreground);
                            const bool becameCurrent = ref.current && !n->current;
                            sourceChanged = sourceChanged || n->current != ref.current; n->current = ref.current;
                            if (n->revision != ref.tip || (n->loading && n->generation != generation_)) {
                                n->revision = ref.tip; n->loaded = n->loading = false; n->count = 0;
                                clearHistoryChildren(item);
                                if (changes_.item(item)->expanded) requestHistory(item);
                            }
                            if (becameCurrent) changes_.expand(item, true);
                            existing.erase(found);
                        }
                    }
                    for (const auto& stale : existing) changes_.erase(stale.second);
                    if (multiple && listing.refs.empty()) insertHistory(parent, L"No branches or commits yet.", {HistoryKind::Message, {}, {}, {}}, false, muted);
                    changes_.sort(parent, [](uintptr_t a, uintptr_t b) {
                        const auto x = reinterpret_cast<HistoryNode*>(a), y = reinterpret_cast<HistoryNode*>(b);
                        return x->current != y->current ? x->current : x->ref < y->ref;
                    });
                }
                if (multiple && fresh) changes_.expand(parent, !collapsedGitPaths_.count(L"repository:" + pathKey(listing.root) + L"\n"));
            }
            for (const auto& stale : repositories) changes_.erase(stale.second);
            branchesLoaded_ = true;
            if (sourceChanged) updateGitFiles(); else layout(false);
        });
    });
}
void ProjectRuntime::Impl::requestHistory(TreeItem item, bool more) {
    auto n = historyNode(item);
    if (n && n->loading && n->generation != generation_) n->loading = false;
    if (!n || n->loading || (n->loaded && !more) || !git_ || n->root.empty()) return;
    if (n->kind != HistoryKind::Branch && n->kind != HistoryKind::Commit) return;
    if (n->kind == HistoryKind::Branch && n->revision.empty()) {
        n->loaded = true; return;
    }
    n->loading = true; n->generation = generation_;
    const auto kind = n->kind; const auto revision = n->revision; const auto count = n->count;
    const auto root = n->root; const auto generation = generation_;
    {
        Tree::Batch batch(changes_);
        if (!more) clearHistoryChildren(item);
        else for (auto child = changes_.child(item); child;) {
            const auto next = changes_.next(child); const auto value = historyNode(child);
            if (value && (value->kind == HistoryKind::More || value->kind == HistoryKind::Message)) changes_.erase(child);
            child = next;
        }
        insertHistory(item, L"Loading…", {HistoryKind::Message, {}, {}, {}}, false, muted);
    }
    gitWorker_->submit(L"history:" + std::to_wstring(item), [this, item, kind, revision, count, root, generation] {
        const auto args = kind == HistoryKind::Branch ? gitLogArguments(revision, count) : gitCommitFilesArguments(revision);
        auto result = provider_->git(gitArguments(args), root, gitWorker_->cancellation());
        auto commits = kind == HistoryKind::Branch ? parseGitCommits(result.output) : std::vector<GitCommit>{};
        auto files = kind == HistoryKind::Commit ? parseGitCommitFiles(result.output) : std::vector<GitCommitFile>{};
        const auto error = result.error; const auto code = result.code;
        gitWorker_->complete([this, item, kind, revision, count, root, generation, commits = std::move(commits), files = std::move(files), code, error] {
            if (generation != generation_) return;
            auto n = historyNode(item); if (!n || n->revision != revision || pathKey(n->root) != pathKey(root)) return;
            n->loading = false; n->loaded = code == 0;
            Tree::Batch batch(changes_);
            for (auto child = changes_.child(item); child;) {
                const auto next = changes_.next(child); const auto value = historyNode(child);
                if (value && value->kind == HistoryKind::Message) changes_.erase(child);
                child = next;
            }
            if (code) {
                insertHistory(item, L"Load failed — click to retry", {HistoryKind::More, {}, revision, {}}, false, muted);
                status(L"Git history: " + error); return;
            }
            if (kind == HistoryKind::Branch) {
                const size_t length = std::min(commits.size(), gitCommitPageSize);
                for (size_t i = 0; i < length; ++i) {
                    const auto& commit = commits[i];
                    const auto text = wide(commit.shortHash + "  " + commit.subject + "  ·  " + commit.author + "  ·  " + commit.date.substr(0, 10));
                    insertHistory(item, text, {HistoryKind::Commit, {}, commit.hash, {}}, true);
                }
                n->count = count + length;
                if (commits.size() > gitCommitPageSize) insertHistory(item, L"Load 50 more commits…", {HistoryKind::More, {}, revision, {}}, false, accent);
                else if (!n->count) insertHistory(item, L"No commits.", {HistoryKind::Message, {}, {}, {}}, false, muted);
            } else {
                n->files.clear();
                for (auto& file : files) {
                    if (root.wsl() && (!validUtf8(file.path) || !validUtf8(file.previousPath))) { status(L"Some commit filenames are not UTF-8 and cannot be opened."); continue; }
                    if (n->files.size() >= maxTreeNodes) { status(L"Commit file tree limited to 50,000 files."); break; }
                    n->files.push_back(std::move(file));
                }
                updateCommitFiles(item);
            }
        });
    });
}
void ProjectRuntime::Impl::updateCommitFiles(TreeItem item) {
    const auto n = historyNode(item); if (!n || n->kind != HistoryKind::Commit) return;
    // Keep the selected file when switching between its folder and path layouts.
    const auto selected = historyNode(changes_.selection());
    auto ancestor = changes_.selection();
    while (ancestor && ancestor != item) ancestor = changes_.item(ancestor)->parent;
    const auto selectedPath = ancestor == item && selected && selected->kind == HistoryKind::File ? selected->path : ResourceId{};
    Tree::Batch batch(changes_); changes_.clearChildren(item);
    std::vector<std::wstring> paths;
    for (const auto& file : n->files) paths.push_back(wide(file.path));
    const auto nodes = buildGitChanges(paths, gitTreeView_); std::vector<TreeItem> items;
    for (const auto& value : nodes) {
        const auto parent = value.parent == noGitChange ? item : items[value.parent];
        const auto offset = value.parent == noGitChange ? 0 : nodes[value.parent].path.size() + 1;
        const auto path = n->root / std::string_view(utf8(value.path));
        const auto child = insertHistory(parent, value.path.substr(offset),
            {value.directory() ? HistoryKind::Directory : HistoryKind::File, {}, n->revision, path}, value.directory(),
            value.directory() ? foreground : stateColor(static_cast<wchar_t>(n->files[value.entry].state)));
        items.push_back(child); if (value.directory()) changes_.expand(child, true);
        if (!selectedPath.empty() && path == selectedPath) changes_.select(child, false);
    }
    if (paths.empty()) insertHistory(item, L"No changed files.", {HistoryKind::Message, {}, {}, {}}, false, muted);
}
void ProjectRuntime::Impl::openHistory(TreeItem item) {
    const auto n = historyNode(item); if (!n) return;
    if (n->kind == HistoryKind::File) openGitDiff(n->path, GitDiffKind::Commit, n->revision, false, n->root);
    else if (n->kind == HistoryKind::More) {
        const auto value = changes_.item(item); if (value) requestHistory(value->parent, true);
    } else if (n->kind == HistoryKind::Branch || n->kind == HistoryKind::Commit) {
        if (!changes_.item(item)->expanded) changes_.expand(item, true);
        else requestHistory(item);
    }
}
void ProjectRuntime::Impl::openWithDefaultApp(ResourceId resource) {
    if (resource.wsl()) { status(L"Open this file from the WSL terminal with the application you want to use."); return; }
    const auto path = resource.local();
    const auto directory = path.parent_path();
    SHELLEXECUTEINFOW execute{}; execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    execute.hwnd = window_; execute.lpFile = path.c_str(); execute.lpDirectory = directory.c_str(); execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        const auto error = winError();
        MessageBoxW(window_, (path.native() + L"\n\n" + error).c_str(), L"Poppy — Open with Default Application", MB_OK | MB_ICONWARNING);
    }
}
void ProjectRuntime::Impl::revealInFileExplorer(ResourceId resource) {
    if (resource.empty()) return;
    auto path = resource.native();
    if (resource.wsl()) {
        // A POSIX backslash is part of a name, not a UNC path separator.
        if (!resource.is_absolute() || path.find(L'\\') != path.npos) {
            MessageBoxW(window_, L"This WSL path cannot be represented in File Explorer.", L"Poppy — Reveal in File Explorer", MB_OK | MB_ICONWARNING);
            return;
        }
        std::replace(path.begin(), path.end(), L'/', L'\\');
        path = L"\\\\wsl.localhost\\" + wide(resource.location().distro) + path;
    }
    PIDLIST_ABSOLUTE item = nullptr;
    auto result = SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr);
    // Let the Windows shell open explorer.exe and select the item without
    // passing filenames through Explorer's command-line parser.
    if (SUCCEEDED(result)) result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    CoTaskMemFree(item);
    if (FAILED(result)) {
        MessageBoxW(window_, (path + L"\n\n" + winError(static_cast<DWORD>(result))).c_str(), L"Poppy — Reveal in File Explorer", MB_OK | MB_ICONWARNING);
    }
}
void ProjectRuntime::Impl::status(std::wstring message) { status_ = std::move(message); statusBar_.message(status_); }
void ProjectRuntime::Impl::updateStatusBar() {
    const std::wstring gitLabel = !git_ ? L"Git" : !git_->error.empty() ? L"Git unavailable" : git_->repositories.empty() ? L"No Git repository" : (git_->repositories.size() == 1 ? git_->repositories.front()->branch : std::to_wstring(git_->repositories.size()) + L" repositories") + L"  ·  " + groupedCount(git_->changed) + L" changed  ·  " + groupedCount(git_->untracked) + L" untracked";
    std::vector<LimitStatus> limits;
    if (!root_.empty()) {
        if (root_.wsl()) { limits.push_back(watchLimitStatus(watchUsage_)); limits.push_back(fileLimitStatus(quickOpen_.index(), quickOpen_.indexing())); }
        limits.push_back(treeLimitStatus(nodes_.size(), maxTreeNodes, maxEntries, partialFolders_));
    }
    statusBar_.editor(viewer().textStatus());
    statusBar_.update(gitLabel, std::move(limits)); statusBar_.message(status_);
}
void ProjectRuntime::Impl::chooseFolder() {
    if (owner_ && owner_->chooseProject) { owner_->chooseProject(); return; }
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST); dialog->SetTitle(L"Open workspace folder");
    if (SUCCEEDED(dialog->Show(window_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { if (owner_ && owner_->openFolder) owner_->openFolder(fs::path(path)); CoTaskMemFree(path); } item->Release();
        }
    }
    dialog->Release();
}
Node* ProjectRuntime::Impl::node(TreeItem item) const { const auto value = explorer_.item(item); return value ? reinterpret_cast<Node*>(value->data) : nullptr; }
wchar_t ProjectRuntime::Impl::stateFor(const ResourceId& path) const {
    if (!git_) return 0;
    const auto direct = git_->states.find(pathKey(path));
    return direct != git_->states.end() ? direct->second : 0;
}
void ProjectRuntime::Impl::updateLabel(TreeItem item) {
    const auto n = node(item); if (!n) return;
    std::wstring text = n->unsupported ? n->label + L"  [unsupported encoding]" : n->path.filename().empty() ? n->path.native() : n->path.filename().native();
    if (n->directory && n->reparse) text += L"  [link]";
    const auto state = stateFor(n->path);
    const auto path = "/" + n->path.generic_u8string() + "/";
    const bool gitMetadata = path.find("/.git/") != std::string::npos;
    explorer_.label(item, std::move(text), n->ignored || gitMetadata ? muted : state ? stateColor(state) : foreground);
}
bool ProjectRuntime::Impl::canPasteImage(TreeItem item) const {
    const auto n = node(item ? item : rootItem_);
    if (!n || n->unsupported || pastePending_ || createPending_ || renamePending_ || gitDiscardPending_ || gitStashPending_ || gitCommitPending_ || (n->directory && n->reparse)) return false;
    return !root_.wsl() || (connected_ && provider_->state() == ConnectionState::Ready);
}
bool ProjectRuntime::Impl::canCreateItem(TreeItem item) const {
    const auto n = node(item);
    return n && n->directory && canPasteImage(item);
}
void ProjectRuntime::Impl::newFile() {
    auto parent = explorer_.selection();
    if (const auto n = node(parent)) {
        if (!n->directory) parent = explorer_.item(parent)->parent;
    } else parent = rootItem_;
    if (!canCreateItem(parent)) return;
    quickOpen_.close(); selectSidebar(Sidebar::Explorer);
    pendingExplorerFile_.clear();
    explorer_.createItem(parent, false);
}
void ProjectRuntime::Impl::createItem(TreeItem parent, const std::wstring& name, bool directory) {
    if (!canCreateItem(parent)) return;
    const auto title = directory ? L"Poppy — New Folder" : L"Poppy — New File";
    std::wstring error;
    const auto path = parseCreationPath(name, directory, root_.wsl(), error);
    if (!path) {
        MessageBoxW(window_, error.c_str(), title, MB_OK | MB_ICONWARNING); return;
    }
    directory = path->directory;
    const auto folder = node(parent)->path;
    auto destination = folder;
    std::vector<ResourceId> parents;
    for (const auto& component : path->components) { parents.push_back(destination); destination = destination / component; }
    createPending_ = true; status(directory ? L"Creating folder…" : L"Creating file…");
    const auto generation = generation_; const auto provider = provider_;
    worker_->submit(L"create-item", [this, provider, folder, destination, parents, directory, generation] {
        std::wstring error;
        ResourceId createdRoot;
        try {
            for (size_t i = 1; i < parents.size() && error.empty(); ++i) {
                bool created = false; error = provider->createDirectory(parents[i], true, &created);
                if (created && createdRoot.empty()) createdRoot = parents[i];
            }
            if (error.empty()) error = directory ? provider->createDirectory(destination) : provider->createFile(destination, {});
            if (error.empty() && createdRoot.empty()) createdRoot = destination;
        }
        catch (const std::exception& failure) { error = wide(failure.what()); }
        worker_->complete([this, folder, destination, parents, directory, generation, error, createdRoot] {
            createPending_ = false; if (generation != generation_) return;
            if (!createdRoot.empty()) recordCreation(createdRoot, error.empty() ? destination : createdRoot);
            if (!error.empty()) {
                refresh(); status(L"Create failed");
                MessageBoxW(window_, (destination.native() + L"\n\n" + error).c_str(), directory ? L"Poppy — New Folder" : L"Poppy — New File", MB_OK | MB_ICONWARNING); return;
            }
            pendingTreeSelection_ = destination;
            for (const auto& path : parents) expandedPaths_[pathKey(path)] = path;
            for (const auto& pair : nodes_) if (const auto n = node(pair.second); n && pathKey(n->path) == pathKey(folder)) { explorer_.expand(pair.second, true); requestDirectory(pair.second); break; }
            refresh();
            if (!directory) openFile(destination);
            status(L"Created " + destination.filename().native());
            if (owner_ && owner_->changed) owner_->changed();
        });
    });
}
void ProjectRuntime::Impl::pasteImage(TreeItem item) {
    if (!canPasteImage(item)) return;
    const auto n = node(item ? item : rootItem_);
    const auto directory = n->directory ? n->path : n->path.parent_path();
    SYSTEMTIME now{}; GetLocalTime(&now);
    std::tm time{}; time.tm_year = now.wYear - 1900; time.tm_mon = now.wMonth - 1; time.tm_mday = now.wDay;
    time.tm_hour = now.wHour; time.tm_min = now.wMinute; time.tm_sec = now.wSecond;
    const auto destination = directory / pastedImageName(time);
    ClipboardImage image;
    try { image = readClipboardImage(window_); }
    catch (const std::exception& error) { MessageBoxW(window_, wide(error.what()).c_str(), L"Poppy — Paste Image", MB_OK | MB_ICONWARNING); return; }
    pastePending_ = true; status(L"Pasting image…");
    const auto generation = generation_; const auto provider = provider_;
    worker_->submit(L"paste-image", [this, provider, destination, directory, generation, image = std::move(image)] {
        std::wstring error;
        try { error = provider->createFile(destination, encodeClipboardPng(image)); }
        catch (const std::exception& failure) { error = wide(failure.what()); }
        worker_->complete([this, destination, directory, generation, error] {
            pastePending_ = false; if (generation != generation_) return;
            if (!error.empty()) {
                status(L"Paste Image failed");
                MessageBoxW(window_, (destination.native() + L"\n\n" + error).c_str(), L"Poppy — Paste Image", MB_OK | MB_ICONWARNING); return;
            }
            recordCreation(destination);
            pendingTreeSelection_ = destination;
            for (const auto& pair : nodes_) if (const auto current = node(pair.second); current && pathKey(current->path) == pathKey(directory)) { requestDirectory(pair.second); break; }
            refresh(); status(L"Created " + destination.filename().native());
            if (owner_ && owner_->changed) owner_->changed();
        });
    });
}
void ProjectRuntime::Impl::renameFile(TreeItem item, const std::wstring& name) {
    const auto n = node(item); if (!n || n->unsupported || item == rootItem_ || renamePending_ || pastePending_ || createPending_ || gitDiscardPending_ || gitStashPending_ || gitCommitPending_) return;
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { status(L"Reconnect WSL before renaming."); return; }
    std::wstring error = L"Enter a valid Linux filename (no /, NUL, . or ..; at most 255 UTF-8 bytes).";
    if (!(root_.wsl() ? validPosixName(utf8(name)) : validFileName(name, error))) { MessageBoxW(window_, error.c_str(), L"Poppy — Rename", MB_OK | MB_ICONWARNING); return; }
    relocateFile(item, n->path.parent_path() / name);
}
bool ProjectRuntime::Impl::canMoveFile(TreeItem item, TreeItem parent) const {
    const auto source = node(item), target = node(parent);
    return explorer_.canDrag && explorer_.canDrag(item) && source && target && target->directory && !target->reparse && !target->unsupported &&
        root_.contains(source->path) && root_.contains(target->path) && !source->path.contains(target->path) &&
        pathKey(source->path.parent_path()) != pathKey(target->path);
}
void ProjectRuntime::Impl::deleteItems(const std::vector<TreeItem>& items) {
    if (items.empty() || !std::all_of(items.begin(), items.end(), [this](TreeItem item) { return explorer_.canDelete(item); })) return;
    std::vector<ExplorerEdit> edits;
    for (const auto item : items) {
        const auto source = node(item)->path;
        ExplorerEdit edit{source, root_.parent_path() / (L".poppy-undo-" + wide(newSessionId())), ExplorerEditKind::Delete, source};
        for (const auto& pair : expandedPaths_) if (source.contains(pair.second)) edit.expanded.push_back(pair.second);
        edits.push_back(std::move(edit));
    }
    executeExplorerBatch(std::move(edits));
}
void ProjectRuntime::Impl::copyItems(const std::vector<TreeItem>& items, bool cut) {
    if (items.empty() || !std::all_of(items.begin(), items.end(), [this, cut](TreeItem item) { return cut ? explorer_.canDrag(item) : explorer_.canCopy(item); })) return;
    std::vector<ResourceId> paths;
    for (const auto item : items) paths.push_back(node(item)->path);
    if (!writeExplorerClipboard(window_, paths, cut)) {
        MessageBoxW(window_, L"Cannot write files to the clipboard.", L"Poppy — Clipboard", MB_OK | MB_ICONWARNING); return;
    }
    status(std::to_wstring(paths.size()) + (cut ? L" item(s) cut" : L" item(s) copied"));
}
void ProjectRuntime::Impl::pasteFiles(TreeItem item) {
    if (!explorer_.canPasteFiles(item)) return;
    auto clipboard = readExplorerClipboard(window_);
    if (clipboard.paths.empty()) return;
    const auto target = node(item ? item : rootItem_);
    if (!target) return;
    const auto directory = target->directory ? target->path : target->path.parent_path();
    const auto parentItem = target->directory ? (item ? item : rootItem_) : explorer_.item(item)->parent;
    if (!canCreateItem(parentItem)) return;
    std::vector<ExplorerEdit> edits;
    std::vector<ResourceId> copies;
    std::set<std::wstring> clipboardPaths, sources;
    for (auto& source : clipboard.paths) { source = source.lexically_normal(); clipboardPaths.insert(pathKey(source)); }
    for (const auto& source : clipboard.paths) {
        if (!source.is_absolute() || !source.sameAuthority(directory) || source.filename().empty() ||
            pathKey(source) == pathKey(root_) || source.contains(directory)) {
            MessageBoxW(window_, L"Choose files or folders from the same Windows or WSL connection. A folder cannot be pasted into itself or its descendants.", L"Poppy — Paste", MB_OK | MB_ICONWARNING); return;
        }
        bool covered = false;
        for (auto parent = source.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            if (clipboardPaths.count(pathKey(parent))) { covered = true; break; }
            if (parent == parent.parent_path()) break;
        }
        if (covered || !sources.insert(pathKey(source)).second) continue;
        if (clipboard.cut && pathKey(source.parent_path()) == pathKey(directory)) continue;
        const auto destination = directory / source.filename().native();
        if (clipboard.cut) edits.push_back({source, destination, ExplorerEditKind::Move});
        else {
            const auto backup = root_.parent_path() / (L".poppy-undo-" + wide(newSessionId()));
            edits.push_back({backup, destination, ExplorerEditKind::Create, destination}); copies.push_back(source);
        }
    }
    if (edits.empty()) return;
    expandedPaths_[pathKey(directory)] = directory;
    executeExplorerBatch(std::move(edits), std::move(copies), std::move(clipboard));
}
void ProjectRuntime::Impl::moveItems(const std::vector<TreeItem>& items, TreeItem parent) {
    if (items.empty() || !std::all_of(items.begin(), items.end(), [this, parent](TreeItem item) { return canMoveFile(item, parent); })) return;
    std::vector<ExplorerEdit> edits;
    for (const auto item : items) edits.push_back({node(item)->path, node(parent)->path / node(item)->path.filename().native(), ExplorerEditKind::Move});
    executeExplorerBatch(std::move(edits));
}
void ProjectRuntime::Impl::executeExplorerBatch(std::vector<ExplorerEdit> edits, std::vector<ResourceId> copies, ExplorerClipboard clipboard) {
    if (edits.empty()) return;
    saveTab(); endTabDrag(); renamePending_ = true;
    const std::wstring action = edits.front().kind == ExplorerEditKind::Delete ? L"Delete" : copies.empty() ? L"Move" : L"Paste";
    status(action + L" — " + std::to_wstring(edits.size()) + L" item(s)…");
    struct BufferVersion { ResourceId path; std::shared_ptr<EditBuffer> buffer; std::string baseline, version; };
    std::vector<BufferVersion> buffers;
    if (root_.wsl() && copies.empty()) for (const auto& edit : edits) if (edit.kind == ExplorerEditKind::Move)
        for (const auto& tab : tabs_) if (tab.buffer && edit.before.contains(tab.path))
            buffers.push_back({relocateResource(tab.path, edit.before, edit.after), tab.buffer, tab.buffer->baseline, {}});
    worker_->submit(L"explorer-batch", [this, edits = std::move(edits), copies = std::move(copies), clipboard = std::move(clipboard), action, buffers = std::move(buffers)]() mutable {
        std::wstring error;
        std::vector<ExplorerEdit> applied;
        std::map<std::wstring, std::set<std::wstring>> copyNames;
        std::set<std::wstring> copyDirectories;
        for (size_t i = 0; i < edits.size(); ++i) {
            auto edit = edits[i];
            try {
                if (!copies.empty()) {
                    // Pasting into the source folder creates a new sibling.
                    // Other name collisions report an error without overwriting.
                    if (pathKey(copies[i].parent_path()) == pathKey(edit.after.parent_path())) {
                        const auto key = pathKey(edit.after.parent_path());
                        if (!copyNames.count(key)) {
                            const auto listing = provider_->listDirectory(edit.after.parent_path());
                            if (!listing.error.empty()) throw std::runtime_error(utf8(listing.error));
                            auto& names = copyNames[key];
                            for (const auto& entry : listing.entries) {
                                names.insert(pathKey(entry.path));
                                if (entry.directory) copyDirectories.insert(pathKey(entry.path));
                            }
                        }
                        auto& existing = copyNames[key];
                        const bool directory = copyDirectories.count(pathKey(copies[i])) != 0;
                        const auto name = copies[i].filename().native(), extension = directory ? std::wstring{} : copies[i].extension().native();
                        const auto base = name.substr(0, name.size() - extension.size());
                        unsigned suffix = 1;
                        do {
                            edit.after = copies[i].parent_path() / (base + L" - Copy" + (suffix == 1 ? std::wstring{} : L" (" + std::to_wstring(suffix) + L")") + extension);
                            ++suffix;
                        } while (existing.count(pathKey(edit.after)));
                        existing.insert(pathKey(edit.after));
                        edit.selection = edit.after;
                    }
                    error = provider_->copyResource(copies[i], edit.after);
                } else error = provider_->renameResource(edit.before, edit.after);
            } catch (const std::exception& failure) { error = wide(failure.what()); }
            if (!error.empty()) { error = (copies.empty() ? edit.before : copies[i]).native() + L"\n\n" + error; break; }
            applied.push_back(std::move(edit));
        }
        for (auto& buffer : buffers) if (std::any_of(applied.begin(), applied.end(), [&](const ExplorerEdit& edit) { return edit.kind == ExplorerEditKind::Move && edit.after.contains(buffer.path); })) {
            try { const auto file = provider_->readFile(buffer.path); if (file.error.empty() && file.bytes == buffer.baseline) buffer.version = file.version; }
            catch (const std::exception&) { /* Preserve successful moves even if the version cannot be read. */ }
        }
        worker_->complete([this, applied = std::move(applied), error, action, clipboard = std::move(clipboard), buffers = std::move(buffers)] {
            renamePending_ = false;
            for (const auto& buffer : buffers) if (!buffer.version.empty() && buffer.buffer->baseline == buffer.baseline) buffer.buffer->fileVersion = buffer.version;
            if (!applied.empty()) {
                discardBackups(explorerHistory_.recordBatch(applied));
                applyExplorerBatch(applied, true);
                if (clipboard.cut && GetClipboardSequenceNumber() == clipboard.sequence) {
                    std::vector<ResourceId> remaining;
                    for (const auto& path : clipboard.paths) if (std::none_of(applied.begin(), applied.end(), [&](const ExplorerEdit& edit) { return edit.before.contains(path); })) remaining.push_back(path);
                    writeExplorerClipboard(window_, remaining, true);
                }
                if (owner_ && owner_->changed) owner_->changed();
            }
            if (!error.empty()) {
                status(action + L" failed");
                MessageBoxW(window_, error.c_str(), (L"Poppy — " + action).c_str(), MB_OK | MB_ICONWARNING);
            } else status(action + L" completed — " + std::to_wstring(applied.size()) + L" item(s)");
        });
    });
}
void ProjectRuntime::Impl::applyExplorerBatch(const std::vector<ExplorerEdit>& edits, bool redo) {
    if (edits.empty()) return;
    Tree::Batch batch(explorer_);
    pendingTreeSelections_.clear();
    std::map<std::wstring, TreeItem> items;
    for (const auto& pair : nodes_) if (const auto n = node(pair.second)) items[pathKey(n->path)] = pair.second;
    std::vector<ResourceId> selected;
    ++generation_; ++serial_[0]; ++serial_[1];
    for (const auto& edit : edits) {
        if (edit.kind == ExplorerEditKind::Move) {
            const auto source = redo ? edit.before : edit.after, destination = redo ? edit.after : edit.before;
            applyRelocation(source, destination, false); selected.push_back(destination);
        } else if (edit.restores(redo)) {
            for (const auto& path : edit.expanded) expandedPaths_[pathKey(path)] = path;
            selected.push_back(edit.selection.empty() ? edit.resource() : edit.selection);
        } else {
            const auto& resource = edit.resource();
            forgetExpanded(resource);
            const auto found = items.find(pathKey(resource));
            if (found != items.end()) explorer_.erase(found->second);
            pendingTreeSelection_ = resource.parent_path();
        }
    }
    for (const auto& pair : nodes_) if (auto n = node(pair.second)) n->loading = false;
    if (!selected.empty()) {
        pendingTreeSelection_.clear(); explorer_.select(0, false);
        for (const auto& path : selected) pendingTreeSelections_[pathKey(path)] = path;
        for (const auto& pair : nodes_) if (const auto n = node(pair.second); n && pendingTreeSelections_.erase(pathKey(n->path))) explorer_.addSelection(pair.second);
        explorer_.showSelection();
    } else {
        const auto found = items.find(pathKey(pendingTreeSelection_));
        if (found != items.end() && explorer_.item(found->second)) { explorer_.select(found->second, false); pendingTreeSelection_.clear(); }
    }
    if (!pendingTreeSelection_.empty()) selected.push_back(pendingTreeSelection_);
    for (const auto& selection : selected) for (auto path = selection.parent_path(); root_.contains(path); path = path.parent_path()) {
        expandedPaths_[pathKey(path)] = path;
        if (path == root_ || path == path.parent_path()) break;
    }
    layout(); refresh();
}
void ProjectRuntime::Impl::relocateFile(TreeItem item, const ResourceId& destination, TreeItem parent) {
    const auto n = node(item); if (!n) return;
    const auto source = n->path;
    if (source.native() == destination.native()) return;
    const bool moving = parent != 0;
    saveTab(); endTabDrag(); renamePending_ = true; status((moving ? L"Moving " : L"Renaming ") + source.filename().native() + L"…");
    const auto generation = generation_;
    struct RenamedBuffer { ResourceId path; std::shared_ptr<EditBuffer> buffer; std::string baseline, version; };
    std::vector<RenamedBuffer> buffers;
    if (root_.wsl()) for (const auto& tab : tabs_) if (tab.buffer && source.contains(tab.path)) buffers.push_back({relocateResource(tab.path, source, destination), tab.buffer, tab.buffer->baseline, {}});
    worker_->submit(L"rename", [this, source, destination, generation, moving, buffers = std::move(buffers)]() mutable {
        // Deliberately omit MOVEFILE_REPLACE_EXISTING: collisions must never overwrite.
        const auto error = provider_->renameResource(source, destination);
        if (error.empty()) for (auto& item : buffers) { const auto file = provider_->readFile(item.path); if (file.error.empty() && file.bytes == item.baseline) item.version = file.version; }
        worker_->complete([this, source, destination, generation, moving, error, buffers = std::move(buffers)] {
            renamePending_ = false; if (generation != generation_) return;
            if (!error.empty()) { status(moving ? L"Move failed" : L"Rename failed"); MessageBoxW(window_, (source.native() + L"\n\n" + error).c_str(), moving ? L"Poppy — Move" : L"Poppy — Rename", MB_OK | MB_ICONWARNING); return; }
            for (const auto& item : buffers) if (!item.version.empty() && item.buffer->baseline == item.baseline) item.buffer->fileVersion = item.version;
            recordExplorer({source, destination, ExplorerEditKind::Move});
            applyRelocation(source, destination);
            status((moving ? L"Moved to " : L"Renamed to ") + (moving ? destination.native() : destination.filename().native()));
            if (owner_ && owner_->changed) owner_->changed();
        });
    });
}
void ProjectRuntime::Impl::applyRelocation(const ResourceId& source, const ResourceId& destination, bool refreshView) {
    TreeItem item = 0, parent = 0;
    for (const auto& pair : nodes_) if (const auto n = node(pair.second)) {
        if (pathKey(n->path) == pathKey(source)) item = pair.second;
        if (pathKey(n->path) == pathKey(destination.parent_path())) parent = pair.second;
    }
    const bool moving = pathKey(source.parent_path()) != pathKey(destination.parent_path());
    saveTab(); ++generation_; ++serial_[0]; ++serial_[1];
    std::map<std::wstring, ResourceId> expanded;
    for (const auto& pair : expandedPaths_) { auto path = relocateResource(pair.second, source, destination); expanded[pathKey(path)] = std::move(path); }
    expandedPaths_ = std::move(expanded); pendingTreeSelection_ = destination;
    pendingExplorerFile_ = relocateResource(pendingExplorerFile_, source, destination);
    for (auto path = destination.parent_path(); root_.contains(path); path = path.parent_path()) {
        expandedPaths_[pathKey(path)] = path;
        if (path == root_ || path == path.parent_path()) break;
    }
    {
        Tree::Batch batch(explorer_);
        if (moving && item) {
            if (parent) explorer_.reparent(item, parent);
            else explorer_.erase(item);
        }
        for (const auto& pair : nodes_) if (auto current = node(pair.second)) {
            current->path = relocateResource(current->path, source, destination); current->loading = false; updateLabel(pair.second);
        }
        for (const auto& pair : nodes_) if (const auto current = node(pair.second); current && current->directory) explorer_.sort(pair.second, explorerLess);
        if (refreshView && moving && node(parent)) requestDirectory(parent);
    }
    for (auto& tab : tabs_) {
        const auto gitRoot = relocateResource(tab.gitRoot, source, destination);
        const bool repositoryMoved = pathKey(gitRoot) != pathKey(tab.gitRoot);
        tab.gitRoot = gitRoot;
        if (tab.diffKind == GitDiffKind::Commit && !repositoryMoved) continue;
        const auto path = relocateResource(tab.path, source, destination);
        if (path == tab.path) continue;
        tab.path = path;
        if (tab.buffer) { tab.buffer->document.language = path.filename() == L"CMakeLists.txt" ? Language::Config : languageFor(utf8(path.extension().native())); indexDocument(tab.buffer->document); }
    }
    terminal_.relocate(source, destination);
    if (refreshView) { layout(); refresh(); }
}
void ProjectRuntime::Impl::discardBackups(std::vector<ResourceId> paths) {
    if (paths.empty()) return;
    const auto provider = provider_;
    worker_->submit(L"undo-cleanup:" + wide(newSessionId()), [this, provider, paths = std::move(paths)] {
        for (const auto& path : paths) {
            const auto error = provider->removeResource(path);
            if (!error.empty()) worker_->complete([this, path, error] { status(L"Cannot remove Undo backup " + path.native() + L": " + error); });
        }
    });
}
void ProjectRuntime::Impl::recordExplorer(ExplorerEdit edit) {
    discardBackups(explorerHistory_.record(std::move(edit)));
}
void ProjectRuntime::Impl::stopExplorerHistory() {
    if (historyStopped_ || !worker_) return;
    historyStopped_ = true;
    worker_->drain(); worker_->dispatch();
    discardBackups(explorerHistory_.clear());
    worker_->drain(); worker_->dispatch();
}
void ProjectRuntime::Impl::recordCreation(const ResourceId& path, const ResourceId& selection) {
    const auto backup = root_.parent_path() / (L".poppy-undo-" + wide(newSessionId()));
    recordExplorer({backup, path, ExplorerEditKind::Create, selection.empty() ? path : selection});
}
bool ProjectRuntime::Impl::canReplayExplorer(bool redo) const {
    return !renamePending_ && !pastePending_ && !createPending_ && !gitStagePending_ && !gitDiscardPending_ && !gitStashPending_ && !gitCommitPending_ &&
        (!root_.wsl() || (connected_ && provider_->state() == ConnectionState::Ready)) && explorerHistory_.next(redo);
}
void ProjectRuntime::Impl::replayExplorer(bool redo) {
    if (!canReplayExplorer(redo)) return;
    saveTab(); endTabDrag();
    auto pending = explorerHistory_.nextBatch(redo);
    for (auto& edit : *pending) if (edit.kind != ExplorerEditKind::Move && !edit.restores(redo)) {
        edit.expanded.clear();
        for (const auto& pair : expandedPaths_) if (edit.resource().contains(pair.second)) edit.expanded.push_back(pair.second);
        if (const auto selected = node(explorer_.selection()); selected && edit.resource().contains(selected->path)) edit.selection = selected->path;
    }
    struct BufferVersion { ResourceId path; std::shared_ptr<EditBuffer> buffer; std::string baseline, version; };
    std::vector<BufferVersion> buffers;
    if (root_.wsl()) for (const auto& edit : *pending) for (const auto& tab : tabs_) if (tab.buffer) {
        const auto source = redo ? edit.before : edit.after, destination = redo ? edit.after : edit.before;
        if (edit.kind == ExplorerEditKind::Move && source.contains(tab.path)) buffers.push_back({relocateResource(tab.path, source, destination), tab.buffer, tab.buffer->baseline, {}});
        else if (edit.kind != ExplorerEditKind::Move && edit.restores(redo) && destination.contains(tab.path)) buffers.push_back({tab.path, tab.buffer, tab.buffer->baseline, {}});
    }
    renamePending_ = true; status(redo ? L"Redoing Explorer operation…" : L"Undoing Explorer operation…");
    worker_->submit(L"explorer-history", [this, redo, buffers = std::move(buffers)]() mutable {
        std::vector<ExplorerEdit> applied;
        const auto error = explorerHistory_.replay(redo, [this](const ResourceId& from, const ResourceId& to) { return provider_->renameResource(from, to); }, &applied);
        for (auto& buffer : buffers) if (std::any_of(applied.begin(), applied.end(), [&](const ExplorerEdit& edit) {
            return (edit.kind == ExplorerEditKind::Move || edit.restores(redo)) && (redo ? edit.after : edit.before).contains(buffer.path);
        })) {
            try { const auto file = provider_->readFile(buffer.path); if (file.error.empty() && file.bytes == buffer.baseline) buffer.version = file.version; }
            catch (const std::exception&) { /* Keep successful filesystem operations when a version read fails. */ }
        }
        worker_->complete([this, redo, applied = std::move(applied), error, buffers = std::move(buffers)] {
            renamePending_ = false;
            for (const auto& buffer : buffers) if (!buffer.version.empty() && buffer.buffer->baseline == buffer.baseline) buffer.buffer->fileVersion = buffer.version;
            if (!applied.empty()) {
                applyExplorerBatch(applied, redo);
                if (owner_ && owner_->changed) owner_->changed();
            }
            if (!error.empty()) {
                status(L"Explorer Undo/Redo failed");
                MessageBoxW(window_, error.c_str(), L"Poppy — Explorer Undo/Redo", MB_OK | MB_ICONWARNING); return;
            }
            status(redo ? L"Explorer operation redone" : L"Explorer operation undone");
        });
    });
}
TreeItem ProjectRuntime::Impl::insert(TreeItem parent, const Entry& entry) {
    if (nodes_.size() >= maxTreeNodes) return 0;
    auto n = std::make_unique<Node>(); n->path = entry.path; n->id = nextId_++; n->directory = entry.directory; n->reparse = entry.reparse; n->unsupported = entry.unsupported; n->label = entry.label;
    const auto item = explorer_.insert(parent, L"", n->directory && !n->reparse, reinterpret_cast<uintptr_t>(n.get()));
    if (item) { nodes_[n->id] = item; n.release(); updateLabel(item); }
    return item;
}
void ProjectRuntime::Impl::openFolder(ResourceId path) {
    quickOpen_.close();
    endTabDrag();
    root_ = std::move(path); provider_ = createProjectProvider(root_); connected_ = !root_.wsl();
    terminal_.setProvider(provider_); quickOpen_.setProvider(provider_);
    searchPane_.setContext(root_, provider_);
    auto saved = std::move(initialState_);
    restoreProject(saved.value_or(ProjectSession{}));
    if (!saved) expandedPaths_[pathKey(root_)] = root_;
    viewer().message(L"Select a file on the left.\n\nEdit here and save with Ctrl+S, or use your AI CLI in the terminal.");
    updateGitFiles();
    rootItem_ = insert(0, {root_, true, false, false, {}});
    if (expandedPaths_.count(pathKey(root_))) requestDirectory(rootItem_);
    if (pathKey(pendingTreeSelection_) == pathKey(root_)) { explorer_.select(rootItem_, false); pendingTreeSelection_.clear(); }
    terminal_.setDirectory(root_); watch(); if (connected_) quickOpen_.setRoot(root_); refreshGit();
    SetWindowTextW(window_, (L"Poppy — " + root_.native()).c_str()); status(L"F5: refresh  ·  F6: diff  ·  Ctrl+J: terminal"); InvalidateRect(window_, nullptr, FALSE);
    if (root_.wsl() && active_) connectRemote();
    if (!tabs_.empty()) { if (activeTab_ < tabs_.size()) { focusedGroup_ = tabs_[activeTab_].group; for (auto& tab : tabs_) if (tab.group == focusedGroup_) tab.displayed = false; tabs_[activeTab_].displayed = true; } normalizeGroups(); layout(false); loadTab(false, 0); loadTab(false, 1); }
}
void ProjectRuntime::Impl::connectRemote() {
    if (connecting_ || !provider_ || !root_.wsl()) return;
    connecting_ = true; connected_ = false; connectionError_.clear();
    status(L"Connecting to WSL · " + wide(root_.location().distro) + L"…");
    if (owner_ && owner_->changed) owner_->changed();
    const auto root = root_; const auto generation = ++generation_;
    worker_->submit(L"connect-wsl", [this, root, generation, expectedUid = wslUid_] {
        ResourceId resolved; std::wstring error;
        try { provider_->reconnect(); provider_->expectIdentity(expectedUid); resolved = provider_->connect(root); }
        catch (const std::exception& failure) { error = wide(failure.what()); }
        worker_->complete([this, resolved, generation, error] {
            if (generation_ != generation) return;
            connecting_ = false; connectionError_ = error;
            if (!error.empty()) { status(L"WSL unavailable. " + error + L"  ·  F5: reconnect"); if (!viewer().hasDocument()) viewer().message(error + L"\n\nPress F5 to reconnect."); }
            else {
                const auto previous = root_; root_ = resolved; connected_ = true; wslUid_ = provider_->identity(); git_.reset(); watchLimited_ = false;
                std::map<std::wstring, ResourceId> expanded;
                for (const auto& item : expandedPaths_) { auto path = relocateResource(item.second, previous, root_); expanded[pathKey(path)] = std::move(path); }
                expandedPaths_ = std::move(expanded); expandedPaths_[pathKey(root_)] = root_;
                pendingTreeSelection_ = relocateResource(pendingTreeSelection_, previous, root_);
                pendingExplorerFile_ = relocateResource(pendingExplorerFile_, previous, root_);
                for (auto& tab : tabs_) { tab.path = relocateResource(tab.path, previous, root_); tab.gitRoot = relocateResource(tab.gitRoot, previous, root_); }
                for (const auto& pair : nodes_) if (auto n = node(pair.second)) { n->path = relocateResource(n->path, previous, root_); n->loading = false; updateLabel(pair.second); }
                terminal_.relocate(previous, root_); terminal_.setDirectory(root_); quickOpen_.setRoot(root_); searchPane_.setContext(root_, provider_);
                fileDirty_ = true; requestDirectory(rootItem_); refresh(false); status(L"WSL connected · " + wide(root_.location().distro));
                if (owner_ && owner_->resolved) owner_->resolved(root_);
            }
            layout(false); if (owner_ && owner_->changed) owner_->changed();
        });
    });
}
ProjectSession ProjectRuntime::Impl::saveSession() {
    saveTab();
    ProjectSession state;
    state.wslUid = wslUid_;
    state.sidebar = sidebar_; state.terminalHeight = terminalHeight_; state.terminalVisible = terminalVisible_;
    state.gitSidebar = activeSidebar_ == Sidebar::Git;
    state.diffInline = viewer().diffInline(); state.gitTree = gitTreeView_; state.firstTabRow = editorGroups_[0].firstRow; state.rightTabRow = editorGroups_[1].firstRow; state.editorRatio = editorRatio_;
    saveEditorTabs(state, tabs_, activeTab_);
    for (const auto& entry : expandedPaths_) state.expanded.push_back(entry.second.u8string());
    if (!pendingExplorerFile_.empty()) state.selected = pendingExplorerFile_.u8string();
    else if (!pendingTreeSelection_.empty()) state.selected = pendingTreeSelection_.u8string();
    else if (const auto selected = node(explorer_.selection())) state.selected = selected->path.u8string();
    state.explorerTop = restoredScroll_ ? explorer_.top() : restoreExplorerTop_; state.explorerLeft = restoredScroll_ ? explorer_.left() : restoreExplorerLeft_;
    state.gitTop = git_ ? changes_.top() : restoreGitTop_; state.gitLeft = git_ ? changes_.left() : restoreGitLeft_;
    if (const auto selected = sourceNode(changes_.selection())) state.gitSelected = utf8(selected->path);
    else state.gitSelected = pendingGitSelection_;
    for (const auto& path : collapsedGitPaths_) state.collapsedGit.push_back(utf8(path));
    const auto focus = GetFocus();
    if (focus == terminal_.window()) focusedPane_ = "terminal";
    else if (focus == tree_) focusedPane_ = "explorer";
    else if (focus == gitTree_) focusedPane_ = "git";
    else if (focus == viewer().window() || IsChild(viewer().window(), focus)) focusedPane_ = "viewer";
    state.focusedPane = focusedPane_;
    terminal_.snapshot(state);
    return state;
}
void ProjectRuntime::Impl::restoreProject(const ProjectSession& state) {
    wslUid_ = state.wslUid;
    focusedPane_ = state.focusedPane == "branch" ? "git" : state.focusedPane;
    editorGroups_[0].firstRow = state.firstTabRow; editorGroups_[1].firstRow = state.rightTabRow; editorRatio_ = state.editorRatio;
    restoreExplorerTop_ = state.explorerTop; restoreExplorerLeft_ = state.explorerLeft;
    restoreGitTop_ = state.gitTop; restoreGitLeft_ = state.gitLeft; pendingGitSelection_ = state.gitSelected;
    for (const auto& path : state.collapsedGit) collapsedGitPaths_.insert(wide(path));
    terminal_.setDirectory(root_); terminal_.restore(state);
    for (auto& view : viewers_) view.setDiffLayout(state.diffInline);
    gitTreeView_ = state.gitTree;
    sidebar_ = state.sidebar; terminalHeight_ = state.terminalHeight; terminalVisible_ = state.terminalVisible;
    activeSidebar_ = (state.branchSidebar || state.gitSidebar) ? Sidebar::Git : Sidebar::Explorer;
    searchActivity_.select(false); searchPane_.active(false);
    explorerActivity_.select(activeSidebar_ == Sidebar::Explorer); gitActivity_.select(activeSidebar_ == Sidebar::Git);
    for (const auto& path : state.expanded) {
        auto directory = root_.at(path).lexically_normal().make_preferred();
        const auto key = pathKey(directory);
        if (root_.contains(directory)) expandedPaths_[key] = std::move(directory);
    }
    pendingTreeSelection_ = root_.at(state.selected);
    for (size_t i = 0; i < state.tabs.size(); ++i) {
        const auto& saved = state.tabs[i];
        const auto path = root_.at(saved.path);
        if (!path.is_absolute()) continue;
        Tab target{path, saved.diff, saved.fileView, saved.diffView, saved.pinned, {}, saved.group, saved.displayed, saved.diffKind, saved.revision, saved.allChanges, {}, {}, false, saved.gitRoot.empty() ? ResourceId{} : root_.at(saved.gitRoot)};
        const auto duplicate = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tabKey(tab) == tabKey(target); });
        if (duplicate != tabs_.end()) continue;
        if (i <= state.activeTab) activeTab_ = tabs_.size();
        tabs_.push_back(std::move(target));
    }
}
void ProjectRuntime::Impl::forgetExpanded(const ResourceId& path) {
    for (auto iterator = expandedPaths_.begin(); iterator != expandedPaths_.end();) {
        if (path.contains(iterator->second)) iterator = expandedPaths_.erase(iterator);
        else ++iterator;
    }
}
void ProjectRuntime::Impl::requestDirectory(TreeItem item) {
    const auto n = node(item); if (!n || !n->directory || n->reparse || (root_.wsl() && !connected_)) return;
    n->expandRequested = true;
    expandedPaths_[pathKey(n->path)] = n->path;
    if (n->loading) return;
    n->loading = true; const auto path = n->path; const auto id = n->id, generation = generation_;
    worker_->submit(L"dir:" + std::to_wstring(id), [this, path, id, generation] {
        auto result = std::make_shared<DirectoryResult>();
        static_cast<ProviderListing&>(*result) = provider_->listDirectory(path);
        if (result->error.empty()) {
            std::vector<std::string> names;
            for (const auto& entry : result->entries) if (!entry.unsupported) names.push_back(entry.path.filename().u8string());
            result->ignored = ignoredNames(*provider_, path, names, worker_->cancellation());
        }
        worker_->complete([this, result, id, generation] {
            if (generation != generation_) return;
            const auto found = nodes_.find(id); if (found == nodes_.end()) return;
            auto n = node(found->second); if (!n) return; n->loading = false;
            if (!n->expandRequested) { n->loaded = false; return; }
            n->loaded = result->error.empty();
            applyDirectory(found->second, n, *result);
            if (n->loaded) explorer_.expand(found->second, true);
            if (!restoredScroll_ && std::none_of(nodes_.begin(), nodes_.end(), [this](const auto& pair) { const auto value = node(pair.second); return value && value->loading; })) {
                explorer_.restoreScroll(restoreExplorerTop_, restoreExplorerLeft_); restoredScroll_ = true;
            }
            if (result->error.empty()) revealPendingFile();
            else if (n->path.contains(pendingExplorerFile_)) pendingExplorerFile_.clear();
        });
    });
}
void ProjectRuntime::Impl::applyDirectory(TreeItem item, Node* directory, const DirectoryResult& result) {
    statusDirty_ = true;
    if (!result.error.empty()) { status(L"Folder: " + result.error); return; }
    bool truncated = result.truncated;
    std::map<std::wstring, TreeItem> existing;
    for (auto child = explorer_.child(item); child; child = explorer_.next(child)) if (const auto n = node(child)) existing[n->unsupported ? L"unsupported:" + n->label : pathKey(n->path)] = child;
    Tree::Batch batch(explorer_);
    for (const auto& entry : result.entries) {
        const auto found = existing.find(entry.unsupported ? L"unsupported:" + entry.label : pathKey(entry.path));
        if (found != existing.end()) {
            auto n = node(found->second);
            if (n && n->directory == entry.directory && n->reparse == entry.reparse && n->unsupported == entry.unsupported) {
                n->ignored = !entry.unsupported && result.ignored.count(entry.path.filename().u8string());
                updateLabel(found->second); existing.erase(found); continue;
            }
            if (n) forgetExpanded(n->path);
            explorer_.erase(found->second); existing.erase(found);
        }
        const auto child = insert(item, entry);
        if (!child) truncated = true;
        else { node(child)->ignored = !entry.unsupported && result.ignored.count(entry.path.filename().u8string()); updateLabel(child); }
    }
    for (const auto& stale : existing) { if (const auto n = node(stale.second)) forgetExpanded(n->path); explorer_.erase(stale.second); }
    explorer_.sort(item, explorerLess);
    bool selectionAdded = false;
    for (auto child = explorer_.child(item); child; child = explorer_.next(child)) {
        const auto n = node(child); if (!n) continue;
        if (n->directory && !n->loaded && !n->loading && expandedPaths_.count(pathKey(n->path))) requestDirectory(child);
        if (pendingTreeSelections_.erase(pathKey(n->path))) { explorer_.addSelection(child); selectionAdded = true; }
        if (!pendingTreeSelection_.empty() && pathKey(n->path) == pathKey(pendingTreeSelection_)) {
            explorer_.select(child, false); pendingTreeSelection_.clear();
        }
    }
    if (selectionAdded) explorer_.showSelection();
    if (directory->truncated != truncated) { if (truncated) ++partialFolders_; else --partialFolders_; directory->truncated = truncated; }
    if (truncated || nodes_.size() >= maxTreeNodes) status(L"Explorer limit: 10,000 items/folder, 50,000 retained nodes.");
}
void ProjectRuntime::Impl::revealActiveFile() {
    pendingExplorerFile_.clear();
    if (activeTab_ >= tabs_.size() || tabs_[activeTab_].allChanges) return;
    const auto& path = tabs_[activeTab_].path;
    if (path.empty() || !root_.contains(path) || pathKey(path) == pathKey(root_)) return;
    pendingTreeSelection_.clear(); pendingTreeSelections_.clear();
    pendingExplorerFile_ = path;
    revealPendingFile();
}
void ProjectRuntime::Impl::revealPendingFile() {
    if (pendingExplorerFile_.empty()) return;
    Tree::Batch batch(explorer_);
    auto item = rootItem_;
    while (const auto n = node(item)) {
        if (pathKey(n->path) == pathKey(pendingExplorerFile_)) {
            if (!n->directory && !n->unsupported) explorer_.select(item, false);
            pendingExplorerFile_.clear(); return;
        }
        if (!n->directory || n->reparse || n->unsupported || !n->path.contains(pendingExplorerFile_)) break;
        if (n->loading) return;
        // Walk only the target's ancestors; never enumerate on the UI thread.
        if (!n->loaded) { requestDirectory(item); return; }
        explorer_.expand(item, true);
        auto child = explorer_.child(item);
        for (; child; child = explorer_.next(child)) {
            const auto value = node(child);
            if (value && !value->unsupported && value->path.contains(pendingExplorerFile_)) break;
        }
        item = child;
    }
    // Missing files and paths beneath links have no selectable Explorer row.
    pendingExplorerFile_.clear();
}
void ProjectRuntime::Impl::normalizeGroups() {
    if (tabs_.empty()) { focusedGroup_ = 0; activeTab_ = 0; return; }
    if (std::none_of(tabs_.begin(), tabs_.end(), [](const Tab& t) { return t.group == 0; })) {
        for (auto& tab : tabs_) tab.group = 0;
        focusedGroup_ = 0; ++serial_[0]; ++serial_[1];
        viewers_[0].loading(L"Loading…"); viewers_[1].message(L"");
        PostMessageW(window_, WM_APP + 14, 0, 0);
    }
    for (int group = 0; group < 2; ++group) {
        bool selected = false;
        for (auto& tab : tabs_) if (tab.group == group && tab.displayed) { if (selected) tab.displayed = false; selected = true; }
    }
    for (int group = 0; group < 2; ++group) if (groupTab(group) == tabs_.size()) {
        const auto found = std::find_if(tabs_.begin(), tabs_.end(), [group](const Tab& tab) { return tab.group == group; });
        if (found != tabs_.end()) found->displayed = true;
    }
    if (groupTab(focusedGroup_) == tabs_.size()) focusedGroup_ = 0;
    activeTab_ = groupTab(focusedGroup_);
}
RECT ProjectRuntime::Impl::tabBounds(size_t index) const { return tabRects_[index]; }
bool ProjectRuntime::Impl::tabVisible(size_t i) const {
    const auto& pane = editorGroups_[tabs_[i].group]; const auto rect = tabBounds(i);
    return rect.top >= pane.tabs.top && rect.bottom <= pane.tabs.bottom && rect.right > rect.left;
}
size_t ProjectRuntime::Impl::tabAt(POINT point) const {
    for (const auto i : visibleTabs_) { const auto rect = tabBounds(i); if (PtInRect(&rect, point)) return i; }
    return tabs_.size();
}
std::optional<TabDrop> ProjectRuntime::Impl::tabDropAt(POINT point) const {
    if (!tabDrag_ || tabDrag_->index >= tabs_.size() || !PtInRect(&editorRect_, point)) return std::nullopt;
    const auto& source = tabs_[tabDrag_->index];
    for (int group = 0; group < 2; ++group) {
        const auto& pane = editorGroups_[group];
        if (!PtInRect(&pane.tabs, point)) continue;
        if (pane.overflow() && point.x >= pane.tabs.right - MulDiv(56, dpi_, 96) && point.y >= pane.tabs.bottom - MulDiv(34, dpi_, 96)) return std::nullopt;
        const int stroke = std::max(2, MulDiv(3, dpi_, 96)), inset = MulDiv(3, dpi_, 96);
        const auto target = [&](size_t position, const RECT& rect, LONG x) { const LONG left = std::clamp<LONG>(x - stroke / 2, pane.tabs.left, std::max(pane.tabs.left, pane.tabs.right - stroke)); return TabDrop{position, {left, rect.top + inset, left + stroke, rect.bottom - inset}, group}; };
        std::optional<TabDrop> result;
        for (const auto i : pane.indices) {
            const auto rect = tabBounds(i);
            if (!tabVisible(i) || point.y < rect.top || point.y >= rect.bottom || tabs_[i].pinned != source.pinned) continue;
            if (point.x < rect.left + (rect.right - rect.left) / 2) return target(i, rect, rect.left);
            result = target(i + 1, rect, rect.right);
        }
        return result;
    }
    const bool split = !editorGroups_[1].indices.empty();
    const int middle = split ? editorSplitter_.left : (editorRect_.left + editorRect_.right) / 2;
    const int group = point.x < middle ? 0 : 1;
    if (group == source.group && split) return std::nullopt;
    if (!split && tabs_.size() < 2) return std::nullopt;
    RECT marker = split ? editorGroups_[group].body : RECT{group ? middle : editorRect_.left, tabsRect_.bottom, group ? editorRect_.right : middle, editorRect_.bottom};
    return TabDrop{tabs_.size(), marker, group};
}
void ProjectRuntime::Impl::updateTabDrag(POINT point) {
    if (!tabDrag_) return;
    auto& drag = *tabDrag_; drag.point = point;
    if (!drag.active) {
        if (std::abs(point.x - drag.origin.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi_) && std::abs(point.y - drag.origin.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi_)) return;
        drag.active = true; SendMessageW(tooltips_, TTM_ACTIVATE, FALSE, 0); SetTimer(window_, tabDragTimerId, 300, nullptr);
    }
    drag.target = tabDropAt(point);
    if (drag.target) {
        const auto rect = drag.target->marker;
        dropPreview_.show(rect);
    } else dropPreview_.hide();
    InvalidateRect(window_, nullptr, FALSE);
}
void ProjectRuntime::Impl::scrollTabDrag() {
    if (!tabDrag_ || !tabDrag_->active) return;
    for (auto& pane : editorGroups_) {
        const auto point = tabDrag_->point;
        if (!pane.overflow() || !PtInRect(&pane.tabs, point) || point.y < pane.tabs.bottom - MulDiv(34, dpi_, 96)) continue;
        const int button = MulDiv(28, dpi_, 96);
        if (point.x >= pane.tabs.right - button && pane.firstRow + pane.rows < pane.starts.size()) ++pane.firstRow;
        else if (point.x >= pane.tabs.right - 2 * button && point.x < pane.tabs.right - button && pane.firstRow) --pane.firstRow;
        else return;
        layout(false); updateTabDrag(point); return;
    }
}
void ProjectRuntime::Impl::endTabDrag(bool drop) {
    if (!tabDrag_) return;
    const auto drag = *tabDrag_; tabDrag_.reset(); KillTimer(window_, tabDragTimerId); dropPreview_.hide();
    if (GetCapture() == window_) ReleaseCapture();
    if (!drag.active) return;
    SendMessageW(tooltips_, TTM_ACTIVATE, TRUE, 0);
    if (drop && drag.target) {
        saveTab(); const auto from = drag.index; auto tab = std::move(tabs_[from]);
        int targetGroup = drag.target->group;
        // Creating a left pane moves the other tabs to the right.
        if (editorGroups_[1].indices.empty() && targetGroup == 0 && drag.target->marker.bottom > tabsRect_.bottom) { for (auto& value : tabs_) value.group = 1; }
        tab.group = targetGroup; tab.displayed = true;
        for (auto& value : tabs_) if (value.group == targetGroup) value.displayed = false;
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
        size_t position = drag.target->position; if (position > from) --position;
        position = std::min(position, tabs_.size());
        if (drag.target->marker.bottom > tabsRect_.bottom) {
            const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& t) { return t.group == targetGroup && (!t.pinned || tab.pinned); });
            position = static_cast<size_t>(found - tabs_.begin());
        }
        tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(position), std::move(tab));
        // Moving a preview to another group replaces that group's clean preview.
        if (tabs_[position].preview) for (size_t i = tabs_.size(); i-- > 0;) {
            if (i != position && tabs_[i].group == targetGroup && tabs_[i].preview) {
                tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < position) --position;
            }
        }
        focusedGroup_ = targetGroup; ++serial_[0]; ++serial_[1];
        normalizeGroups(); layout(); loadTab(false, 0); loadTab(false, 1); SetFocus(viewer().window()); return;
    }
    layout();
}
void ProjectRuntime::Impl::updateTabs(bool reveal) {
    statusDirty_ = true;
    if (reveal) { layout(true); return; }
    for (size_t i = 0; i < tabTools_; ++i) { TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.hwnd = window_; tip.uId = 100 + i; SendMessageW(tooltips_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tip)); }
    visibleTabs_.clear();
    for (size_t i = 0; i < tabs_.size(); ++i) if (i < tabRects_.size() && tabVisible(i)) visibleTabs_.push_back(i);
    tabTools_ = visibleTabs_.size();
    for (size_t j = 0; j < visibleTabs_.size(); ++j) { TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_SUBCLASS; tip.hwnd = window_; tip.uId = 100 + j; tip.rect = tabBounds(visibleTabs_[j]); tip.lpszText = LPSTR_TEXTCALLBACKW; SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip)); }
    InvalidateRect(window_, nullptr, FALSE);
}
void ProjectRuntime::Impl::saveTab() {
    for (int group = 0; group < 2; ++group) {
        const auto index = groupTab(group); auto& view = viewers_[group];
        if (index >= tabs_.size() || !view.hasDocument()) continue;
        auto& tab = tabs_[index]; (tab.diff ? tab.diffView : tab.fileView) = view.state();
        if (view.buffer()) tab.buffer = view.buffer();
    }
    keepModifiedEditorTabs(tabs_);
}
std::shared_ptr<EditBuffer> ProjectRuntime::Impl::shareBuffer(const ResourceId& path, std::shared_ptr<EditBuffer> loaded) {
    std::shared_ptr<EditBuffer> buffer;
    for (const auto& tab : tabs_) if (tab.path == path && tab.buffer) { buffer = tab.buffer; break; }
    if (!buffer) buffer = std::move(loaded);
    else if (loaded && !buffer->dirty() && !buffer->saving && (buffer->baseline != loaded->baseline || buffer->fileVersion != loaded->fileVersion)) {
        *buffer = std::move(*loaded);
        for (auto& view : viewers_) if (view.buffer() == buffer) view.refreshBuffer();
    }
    if (buffer) for (auto& tab : tabs_) if (tab.path == path && !tab.allChanges && (tab.diffKind == GitDiffKind::Automatic || tab.diffKind == GitDiffKind::Changes)) tab.buffer = buffer;
    keepModifiedEditorTabs(tabs_);
    return buffer;
}
bool ProjectRuntime::Impl::saveFile(size_t index, bool wait) {
    if (gitDiscardPending_ || gitStashPending_ || gitCommitPending_) {
        if (wait) { gitWorker_->drain(); gitWorker_->dispatch(); }
        if (gitDiscardPending_ || gitStashPending_ || gitCommitPending_) { status(L"Git operation in progress. Save again when it finishes."); return false; }
    }
    if (renamePending_) { if (wait) { worker_->drain(); worker_->dispatch(); } else { status(L"Rename in progress. Save again when it finishes."); return false; } }
    saveTab(); if (index >= tabs_.size()) return true;
    const auto buffer = tabs_[index].buffer; const auto path = tabs_[index].path;
    if (!buffer) return true;
    if (wait && buffer->saving) { worker_->drain(); worker_->dispatch(); }
    if (!buffer->dirty()) return true;
    if (buffer->saving) return false;
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { status(L"Reconnect WSL before saving. Your edits have been kept."); return false; }
    const auto bytes = buffer->encoded(), text = buffer->document.text, baseline = buffer->baseline;
    buffer->saving = true;
    worker_->submit(L"save:" + pathKey(path), [this, path, buffer, bytes, text, baseline, version = buffer->fileVersion] {
        const auto result = provider_->saveFile(path, bytes, version, baseline);
        const auto error = result.error;
        worker_->complete([this, path, buffer, bytes, text, error, version = result.version] {
            buffer->saving = false;
            if (error.empty()) { buffer->saved(bytes, text); buffer->fileVersion = version; status(L"Saved " + path.filename().native()); refreshGit(); if (activeSidebar_ == Sidebar::Search) searchPane_.refresh(); }
            else { status(L"Save failed: " + path.filename().native()); MessageBoxW(window_, (path.native() + L"\n\n" + error).c_str(), L"Poppy — Save", MB_OK | MB_ICONWARNING); }
            updateTabs(false); for (auto& view : viewers_) InvalidateRect(view.window(), nullptr, FALSE);
        });
    });
    if (wait) { worker_->drain(); worker_->dispatch(); return !buffer->dirty(); }
    return true;
}
bool ProjectRuntime::Impl::confirmTab(size_t index) {
    saveTab(); if (index >= tabs_.size()) return true;
    const auto buffer = tabs_[index].buffer;
    if (!buffer) return true;
    if (buffer->saving) { worker_->drain(); worker_->dispatch(); }
    if (!buffer->dirty()) return true;
    const auto answer = MessageBoxW(window_, (L"Save changes to " + tabs_[index].path.filename().native() + L"?").c_str(), L"Poppy", MB_YESNOCANCEL | MB_ICONQUESTION);
    return answer == IDNO || (answer == IDYES && saveFile(index, true));
}
void ProjectRuntime::Impl::openFile(const ResourceId& path, bool diff) {
    if (path.empty()) { status(L"This filename cannot be represented as UTF-8 and cannot be opened or renamed."); return; }
    endTabDrag();
    const auto key = pathKey(path);
    const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tab.diffKind == GitDiffKind::Automatic && pathKey(tab.path) == key; });
    size_t index = static_cast<size_t>(found - tabs_.begin());
    if (found != tabs_.end() && index == activeTab_ && (!diff || found->diff)) { revealActiveFile(); updateTabs(); return; }
    saveTab();
    if (found == tabs_.end()) { Tab tab; tab.path = path; tab.diff = diff; tab.group = focusedGroup_; index = openEditorPreview(tabs_, std::move(tab)); }
    else if (diff) found->diff = true;
    focusedGroup_ = tabs_[index].group; for (auto& tab : tabs_) if (tab.group == focusedGroup_) tab.displayed = false; tabs_[index].displayed = true; activeTab_ = index; layout(); loadTab();
}
void ProjectRuntime::Impl::openContentResult(const ContentMatch& match, const std::string& query, bool matchCase) {
    const auto path = (root_ / std::string_view(match.relative)).lexically_normal();
    if (!root_.contains(path)) { status(L"Invalid search result path."); return; }
    endTabDrag(); saveTab();
    auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tab.diffKind == GitDiffKind::Automatic && tab.path == path; });
    size_t index = static_cast<size_t>(found - tabs_.begin());
    if (found == tabs_.end()) { Tab tab; tab.path = path; tab.group = focusedGroup_; index = openEditorPreview(tabs_, std::move(tab)); }
    auto& tab = tabs_[index]; tab.diff = false; tab.searchTarget = match; tab.searchQuery = query; tab.searchMatchCase = matchCase;
    focusedGroup_ = tab.group;
    for (auto& current : tabs_) if (current.group == focusedGroup_) current.displayed = false;
    tab.displayed = true; activeTab_ = index; layout(); loadTab(false, focusedGroup_); SetFocus(viewer().window());
}
void ProjectRuntime::Impl::applyContentResult(size_t index) {
    if (index >= tabs_.size() || !tabs_[index].searchTarget) return;
    auto& tab = tabs_[index]; const auto match = *tab.searchTarget; tab.searchTarget.reset();
    if (!viewers_[tab.group].selectSearch(match.begin, match.end, tab.searchQuery, tab.searchMatchCase))
        status(L"File changed since the search. Press F5 in Search to refresh the results.");
}
void ProjectRuntime::Impl::openGitDiff(const ResourceId& path, GitDiffKind kind, std::string revision, bool allChanges, ResourceId root) {
    if (path.empty()) return;
    endTabDrag(); saveTab();
    Tab target; target.path = path; target.diff = true; target.diffKind = kind; target.revision = std::move(revision); target.allChanges = allChanges; target.group = focusedGroup_; target.gitRoot = std::move(root);
    const size_t index = openEditorPreview(tabs_, std::move(target));
    focusedGroup_ = tabs_[index].group;
    for (auto& tab : tabs_) if (tab.group == focusedGroup_) tab.displayed = false;
    tabs_[index].displayed = true; activeTab_ = index; layout(); loadTab();
}
void ProjectRuntime::Impl::selectTab(size_t index) {
    endTabDrag();
    if (index >= tabs_.size()) return;
    if (index == activeTab_) { revealActiveFile(); return; }
    saveTab(); focusedGroup_ = tabs_[index].group; for (auto& tab : tabs_) if (tab.group == focusedGroup_) tab.displayed = false; tabs_[index].displayed = true; activeTab_ = index; updateTabs(); loadTab();
}
void ProjectRuntime::Impl::closeTab(size_t index) {
    closeTabs({index});
}
void ProjectRuntime::Impl::closeTabs(const std::vector<size_t>& indices) {
    endTabDrag();
    std::vector<bool> closing(tabs_.size(), false);
    for (const auto index : indices) if (index < tabs_.size() && !tabs_[index].pinned) closing[index] = true;
    if (std::none_of(closing.begin(), closing.end(), [](bool value) { return value; })) return;
    // Finish every save/discard decision before removing tabs. Cancel or a
    // failed save leaves the whole set open, including its unsaved buffers.
    std::set<const EditBuffer*> confirmed;
    for (size_t i = 0; i < tabs_.size(); ++i) if (closing[i] && (!tabs_[i].buffer || confirmed.insert(tabs_[i].buffer.get()).second) && !confirmTab(i)) return;
    saveTab(); bool active = false;
    for (int group = 0; group < 2; ++group) {
        const auto selected = groupTab(group);
        if (selected >= tabs_.size() || !closing[selected]) continue;
        active = true;
        size_t replacement = tabs_.size();
        for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].group == group && !closing[i]) { replacement = i; if (i >= selected) break; }
        if (replacement < tabs_.size()) tabs_[replacement].displayed = true;
    }
    for (size_t i = tabs_.size(); i-- > 0;) if (closing[i]) tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));
    if (activeSidebar_ == Sidebar::Search) searchPane_.refresh();
    layout();
    if (active) {
        pendingExplorerFile_.clear();
        ++serial_[0]; ++serial_[1];
        if (!tabs_.empty()) { loadTab(false, 0); loadTab(false, 1); }
        else { viewer().message(L"Select a file on the left.\n\nEdit here and save with Ctrl+S, or use your AI CLI in the terminal."); status(L"Ready"); }
    }
    SetFocus(viewer().window());
}
void ProjectRuntime::Impl::toggleTabPin(size_t index) {
    endTabDrag();
    if (index >= tabs_.size()) return;
    saveTab();
    const auto activeKey = tabKey(tabs_[activeTab_]);
    auto tab = std::move(tabs_[index]);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    tab.pinned = !tab.pinned;
    tab.preview = false;
    const auto position = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& value) { return value.group == tab.group && !value.pinned; });
    tabs_.insert(position, std::move(tab));
    activeTab_ = static_cast<size_t>(std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& value) { return tabKey(value) == activeKey; }) - tabs_.begin());
    layout();
}
void ProjectRuntime::Impl::tabMenu(POINT screen) {
    endTabDrag();
    if (tabs_.empty()) return;
    size_t index;
    if (screen.x == -1 && screen.y == -1) {
        updateTabs(); index = activeTab_;
        const auto rect = tabBounds(index);
        screen = {rect.left, rect.bottom}; ClientToScreen(window_, &screen);
    } else {
        auto point = screen; ScreenToClient(window_, &point); index = tabAt(point);
        if (index >= tabs_.size()) return;
    }
    const auto path = tabs_[index].path;
    const auto key = tabKey(tabs_[index]);
    const bool pinned = tabs_[index].pinned;
    const int group = tabs_[index].group;
    bool closeOthers = false, closeAll = false;
    for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].group == group && !tabs_[i].pinned) {
        closeAll = true;
        if (i != index) closeOthers = true;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, pinned ? L"Unpin tab" : L"Pin tab");
    if (!tabs_[index].allChanges && tabs_[index].diffKind != GitDiffKind::Commit) {
        AppendMenuW(menu, MF_STRING, 4, L"Reveal in File Explorer");
        AppendMenuW(menu, MF_STRING, 3, L"Open with Default Application");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (pinned ? MF_GRAYED : 0), 2, L"Close tab");
    AppendMenuW(menu, MF_STRING | (closeOthers ? 0 : MF_GRAYED), 5, L"Close Others");
    AppendMenuW(menu, MF_STRING | (closeAll ? 0 : MF_GRAYED), 6, L"Close All");
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(menu);
    const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tabKey(tab) == key; });
    if (found != tabs_.end()) {
        index = static_cast<size_t>(found - tabs_.begin());
        if (command == 1) toggleTabPin(index);
        else if (command == 2) closeTab(index);
        else if (command == 3) openWithDefaultApp(path);
        else if (command == 4) revealInFileExplorer(path);
        else if (command == 5 || command == 6) {
            std::vector<size_t> indices;
            for (size_t i = 0; i < tabs_.size(); ++i)
                if (tabs_[i].group == tabs_[index].group && (command == 6 || i != index)) indices.push_back(i);
            closeTabs(indices);
        }
    }
}
void ProjectRuntime::Impl::loadTab(bool preserve, int group) {
    if (group < 0) group = focusedGroup_;
    if (!preserve && group == focusedGroup_) revealActiveFile();
    waitingForGit_[group] = false;
    const size_t index = groupTab(group); if (index >= tabs_.size()) { viewers_[group].message(L"Select a file."); return; }
    preserve = preserve && viewers_[group].hasDocument();
    const auto& tab = tabs_[index]; const auto path = tab.path;
    if (!tab.allChanges && (tab.diffKind == GitDiffKind::Automatic || tab.diffKind == GitDiffKind::Changes)) shareBuffer(path);
    const auto state = tab.diff ? tab.diffView : tab.fileView;
    const auto serial = ++serial_[group], generation = generation_;
    if (!tab.diff && tab.buffer && (tab.buffer->dirty() || tab.buffer->saving)) {
        viewers_[group].edit(tab.buffer, path, preserve); if (!preserve) viewers_[group].restore(state);
        refreshEditorGit(group);
        status(L"Unsaved changes kept  ·  Ctrl+S: save"); applyContentResult(index); return;
    }
    if (tab.diff && tab.buffer && tab.diffBefore && (tab.buffer->dirty() || tab.buffer->saving)) {
        viewers_[group].editDiff(tab.buffer, tab.diffBefore, path, preserve); if (!preserve) viewers_[group].restore(state);
        preserve = true;
    }
    if (root_.wsl() && !connected_) { if (!viewers_[group].hasDocument()) viewers_[group].message(connecting_ ? L"Connecting to WSL…" : L"WSL disconnected. Press F5 to reconnect.", path); return; }
    // Retain only tab metadata and view positions; reload the active document.
    if (!preserve) viewers_[group].loading(tab.diff ? L"Loading Git diff…" : L"Loading…", path);
    status((tab.diff ? L"Git diff: " : L"") + path.filename().native());
    if (!tab.diff) {
        worker_->submit(L"file:" + std::to_wstring(group), [this, group, path, serial, generation, preserve, state] {
            auto result = readDocument(*provider_, path);
            worker_->complete([this, group, result, path, serial, generation, preserve, state] {
                if (generation != generation_ || serial != serial_[group]) return;
                if (viewers_[group].buffer() && (viewers_[group].buffer()->dirty() || viewers_[group].buffer()->saving)) return;
                if (result->image) {
                    tabs_[groupTab(group)].buffer.reset();
                    viewers_[group].showImage(result->image, path);
                    status(std::to_wstring(result->image->width) + L" × " + std::to_wstring(result->image->height) + L" px  ·  READ ONLY  ·  F6: diff");
                } else if (result->document) {
                    const auto buffer = shareBuffer(path, result->buffer);
                    viewers_[group].edit(buffer, path, preserve); if (!preserve) viewers_[group].restore(state);
                    refreshEditorGit(group);
                    status(std::to_wstring(result->document->lines.size()) + L" lines  ·  Ctrl+S: save  ·  F6: diff");
                    applyContentResult(groupTab(group));
                } else { viewers_[group].message(result->error, path); status(L"Preview unavailable"); }
            });
        });
        return;
    }
    if (!git_) { waitingForGit_[group] = true; viewers_[group].loading(L"Loading Git repository…", path); return; }
    const auto repo = git_->repository(tab.gitRoot.empty() ? path : tab.gitRoot);
    if (!repo || !repo->error.empty() || (!tab.gitRoot.empty() && pathKey(repo->root) != pathKey(tab.gitRoot))) { viewers_[group].message(L"Git repository unavailable for this file. Press F5 to refresh.", path); return; }
    const auto root = tab.gitRoot.empty() ? repo->root : tab.gitRoot;
    const auto kind = tab.diffKind; const auto revision = tab.revision; const auto allChanges = tab.allChanges;
    const bool unsaved = tab.buffer && (tab.buffer->dirty() || tab.buffer->saving);
    const auto roots = git_->roots; const bool treeView = gitTreeView_;
    gitWorker_->submit(L"diff:" + std::to_wstring(group), [this, group, serial, generation, path, root, preserve, state, kind, revision, allChanges, roots, unsaved, treeView] {
        constexpr size_t maxPatchBytes = 4 * 1024 * 1024;
        const auto relative = allChanges ? std::string{} : path.lexically_relative(root).generic_u8string();
        auto readDiff = [&](GitDiffKind scope) {
            std::string previousPath;
            if (!relative.empty() && scope == GitDiffKind::Commit) {
                auto files = provider_->git(gitArguments(gitCommitFilesArguments(revision)), root, gitWorker_->cancellation());
                if (files.code) return files;
                for (const auto& file : parseGitCommitFiles(files.output)) if (file.path == relative) { previousPath = file.previousPath; break; }
            } else if (!relative.empty() && scope == GitDiffKind::Staged) {
                auto status = provider_->git(gitArguments(gitStatusArguments()), root, gitWorker_->cancellation());
                if (status.code) return status;
                for (const auto& entry : parseGitStatus(status.output)) if (entry.path == relative && entry.index == 'R') { previousPath = entry.previousPath; break; }
            }
            if (root.wsl() && !validUtf8(previousPath)) return ProcessResult{1, {}, L"The rename source is not a UTF-8 path."};
            return provider_->git(gitArguments(gitDiffArguments(scope, revision, relative, previousPath)), root, gitWorker_->cancellation());
        };
        auto scope = kind == GitDiffKind::Automatic ? GitDiffKind::Changes : kind;
        auto result = readDiff(scope);
        if (kind == GitDiffKind::Automatic && !unsaved && result.code == 0 && result.output.empty()) { scope = GitDiffKind::Staged; result = readDiff(scope); }
        std::shared_ptr<FileResult> working;
        std::shared_ptr<Document> before;
        if (!allChanges && scope == GitDiffKind::Changes) {
            working = readDocument(*provider_, path);
            // A missing working file remains a read-only deletion preview.
            if (working->document) before = readWorkingBase(*provider_, root, relative, working->document->language, gitWorker_->cancellation());
        }
        std::vector<ResourceId> untracked;
        std::vector<std::wstring> paths;
        if (!result.code && allChanges) {
            // Use the sidebar's input order, including rename destinations and
            // untracked files, before applying its Tree/List layout.
            auto files = provider_->git(gitArguments(scope == GitDiffKind::Commit ? gitCommitFilesArguments(revision) : gitStatusArguments()), root, gitWorker_->cancellation());
            if (files.code) { result.code = files.code; result.error = files.error; }
            else if (scope == GitDiffKind::Commit) {
                for (const auto& file : parseGitCommitFiles(files.output)) {
                    if (root.wsl() && (!validUtf8(file.path) || !validUtf8(file.previousPath))) continue;
                    paths.push_back(wide(file.path));
                    if (paths.size() >= maxTreeNodes) break;
                }
            } else for (const auto& entry : parseGitStatus(files.output)) {
                if (scope == GitDiffKind::Staged ? !entry.staged() : !entry.changed()) continue;
                if (root.wsl() && (!validUtf8(entry.path) || !validUtf8(entry.previousPath))) continue;
                const auto file = (root / std::string_view(entry.path)).lexically_normal();
                if (entry.index == '?' && pathKey(gitRepositoryFor(roots, file)) != pathKey(root)) continue;
                paths.push_back(file.lexically_relative(root).generic_wstring());
                if (entry.index == '?') untracked.push_back(file);
                if (paths.size() >= maxTreeNodes) break;
            }
        }
        for (const auto& file : untracked) {
            if (result.code || *gitWorker_->cancellation()) break;
            auto read = provider_->readFile(file);
            if (!read.error.empty()) { result.code = 1; result.error = read.error; break; }
            std::string text, encoding, error;
            const auto name = file.lexically_relative(root).generic_u8string();
            std::string patch;
            if (!decodeText(read.bytes, text, encoding, error)) patch = "diff --git a/" + name + " b/" + name + "\nBinary file or unsupported text encoding\n";
            else patch = untrackedPatch(name, text);
            if (patch.size() > maxPatchBytes || result.output.size() > maxPatchBytes - patch.size()) { result.code = 1; result.error = L"Combined diff exceeds the 4 MiB preview limit."; break; }
            result.output += patch;
        }
        auto doc = std::make_shared<DiffDocument>();
        if (result.code == 0 && static_cast<size_t>(std::count(result.output.begin(), result.output.end(), '\n')) >= maxLines) { result.code = 1; result.error = L"Diff exceeds the 1,000,000-line preview limit."; }
        if (result.code == 0) {
            doc->patch.text = std::move(result.output); indexDiff(*doc, true);
            if (allChanges) {
                std::vector<std::string> order;
                for (const auto& node : buildGitChanges(paths, treeView)) if (!node.directory()) order.push_back(utf8(node.path));
                orderDiffFiles(*doc, order);
            }
        }
        const auto error = result.error; const auto code = result.code;
        gitWorker_->complete([this, group, serial, generation, path, doc, code, error, scope, preserve, state, allChanges, working, before] {
            if (generation != generation_ || serial != serial_[group]) return;
            const auto index = groupTab(group);
            if (before && working && working->buffer) {
                const auto buffer = shareBuffer(path, working->buffer); tabs_[index].diffBefore = before;
                viewers_[group].editDiff(buffer, before, path, preserve); if (!preserve) viewers_[group].restore(state);
                status(L"Working tree diff · Ctrl+S: save · F7: split / inline"); return;
            }
            // Failed or stale refreshes must not replace an unsaved editor.
            if (viewers_[group].buffer() && (viewers_[group].buffer()->dirty() || viewers_[group].buffer()->saving)) return;
            if (code) viewers_[group].message(error, path);
            else if (doc->patch.text.empty()) viewers_[group].message(L"No changes to display.", path);
            else if (doc->rows.empty()) viewers_[group].message(L"No text changes to display.", path);
            else { viewers_[group].showDiff(doc, path, preserve); if (!preserve) viewers_[group].restore(state); }
            status((scope == GitDiffKind::Commit ? L"Commit diff" : scope == GitDiffKind::Staged ? L"Staged diff" : L"Working tree diff") +
                std::wstring(allChanges ? L" · All changes" : L"") + L" · F7: split / inline");
        });
    });
}
void ProjectRuntime::Impl::refreshEditorGit(int group) {
    const auto request = ++editorGitSerial_[group];
    const auto index = groupTab(group);
    if (index >= tabs_.size() || tabs_[index].diff || !viewers_[group].buffer()) return;
    const auto path = tabs_[index].path;
    const auto repo = git_ ? git_->repository(path) : nullptr;
    if (!repo || !repo->error.empty() || (root_.wsl() && !connected_)) { viewers_[group].setGitBase({}); return; }
    GitStatusEntry entry; entry.path = path.lexically_relative(repo->root).generic_u8string();
    for (const auto& change : repo->entries) if (change.path == path) {
        entry.index = change.index; entry.worktree = change.worktree; entry.previousPath = change.previousPath; break;
    }
    const auto serial = serial_[group], generation = generation_;
    const auto root = repo->root;
    const auto language = viewers_[group].buffer()->document.language;
    gitWorker_->submit(L"editor-git:" + std::to_wstring(group), [this, group, path, root, entry, language, serial, generation, request] {
        const auto bytes = gitEditorBase(entry, [&](const auto& args) -> std::optional<std::string> {
            const auto result = provider_->git(gitArguments(args), root, gitWorker_->cancellation());
            if (result.code) return {};
            return result.output;
        });
        std::shared_ptr<Document> before;
        if (bytes) {
            auto document = std::make_shared<Document>(); document->language = language; std::string error;
            if (decodeText(*bytes, document->text, document->encoding, error) &&
                static_cast<size_t>(std::count(document->text.begin(), document->text.end(), '\n')) < maxLines) {
                indexDocument(*document); before = std::move(document);
            }
        }
        gitWorker_->complete([this, group, path, serial, generation, request, before] {
            if (generation != generation_ || serial != serial_[group] || request != editorGitSerial_[group]) return;
            const auto index = groupTab(group);
            if (index < tabs_.size() && !tabs_[index].diff && tabs_[index].path == path) viewers_[group].setGitBase(before);
        });
    });
}
void ProjectRuntime::Impl::showDiff() {
    if (tabs_.empty()) { status(L"Select a file to view its Git diff."); return; }
    auto& tab = tabs_[activeTab_];
    if (tab.diffKind != GitDiffKind::Automatic) {
        if (!tab.allChanges && tab.diffKind != GitDiffKind::Commit) { const auto path = tab.path; openFile(path); }
        else status(L"This Git comparison is read only. F7: split / inline.");
        return;
    }
    if (!tab.diff && (!git_ || !git_->repository(tab.path) || !git_->repository(tab.path)->error.empty())) { status(L"Git repository unavailable. Check Git in the project environment and press F5."); return; }
    saveTab(); tab.diff = !tab.diff; layout(); loadTab();
}
void ProjectRuntime::Impl::refreshGit(bool rescan) {
    if (root_.empty() || (root_.wsl() && !connected_)) return;
    if (rescan) { gitScanDirty_ = true; ++gitScanRevision_; }
    const auto scanRevision = gitScanRevision_;
    const auto generation = generation_; const auto root = root_; const auto cached = gitScanDirty_ ? nullptr : git_;
    // Directory refreshes classify new entries themselves. Git-only updates
    // must also recheck retained entries (ignore rules or the index may change).
    struct IgnoreGroup { ResourceId directory; std::vector<std::string> names; std::vector<uint64_t> ids; };
    std::map<std::wstring, IgnoreGroup> ignoreGroups;
    if (!rescan) for (const auto& pair : nodes_) if (const auto n = node(pair.second); n && !n->unsupported && pair.second != rootItem_) {
        const auto directory = n->path.parent_path();
        auto& group = ignoreGroups[pathKey(directory)]; group.directory = directory;
        group.names.push_back(n->path.filename().u8string()); group.ids.push_back(n->id);
    }
    gitWorker_->submit(L"git", [this, root, generation, cached, scanRevision, ignoreGroups = std::move(ignoreGroups)] {
        auto result = readGitWorkspace(*provider_, root, gitWorker_->cancellation(), cached ? &cached->scan : nullptr);
        std::vector<std::pair<uint64_t, bool>> ignored;
        for (const auto& pair : ignoreGroups) {
            const auto& group = pair.second;
            const auto names = ignoredNames(*provider_, group.directory, group.names, gitWorker_->cancellation());
            for (size_t i = 0; i < group.names.size(); ++i) ignored.emplace_back(group.ids[i], names.count(group.names[i]) != 0);
        }
        gitWorker_->complete([this, generation, result, scanRevision, ignored = std::move(ignored)] {
            if (generation != generation_) return;
            if (scanRevision == gitScanRevision_) gitScanDirty_ = false;
            git_ = result;
            std::vector<GitCommitPane::Repository> repositories;
            for (const auto& repo : result->repositories) if (repo->error.empty()) repositories.push_back({repo->root, repositoryLabel(repo->root)});
            gitCommit_.repositories(std::move(repositories));
            statusDirty_ = true;
            if (result->repositories.empty()) {
                changes_.clear(); branchesLoaded_ = branchesLoading_ = false; branchError_.clear(); ++branchRequest_;
            }
            {
                Tree::Batch batch(explorer_);
                for (const auto& value : ignored) {
                    const auto found = nodes_.find(value.first);
                    if (found != nodes_.end()) if (const auto n = node(found->second)) n->ignored = value.second;
                }
                for (const auto& pair : nodes_) updateLabel(pair.second);
            }
            updateGitFiles();
            refreshBranches();
            for (int group = 0; group < 2; ++group) {
                if (waitingForGit_[group]) loadTab(false, group);
                else refreshEditorGit(group);
            }
            if (!result->error.empty()) status(L"Git: " + result->error);
            else if (!result->warning.empty()) status(L"Git repository scan: " + result->warning);
            else if (result->unsupportedNames) status(L"Some Git filenames are not UTF-8 and cannot be opened.");
            else if (result->truncated) status(L"Git repository scan or change display reached its limit.");
            InvalidateRect(window_, nullptr, FALSE);
        });
    });
}
void ProjectRuntime::Impl::refresh(bool force) {
    if (force && activeSidebar_ == Sidebar::Search) searchPane_.refresh();
    if (root_.empty()) return;
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { if (force) connectRemote(); return; }
    const bool files = force || filesDirty_;
    if (files) quickOpen_.refresh();
    filesDirty_ = false;
    if (files) {
        for (const auto& pair : nodes_) if (auto n = node(pair.second); n && n->directory) n->loaded = false;
        for (const auto& pair : nodes_) {
            const auto n = node(pair.second); if (!n || !n->expandRequested) continue;
            auto item = explorer_.item(pair.second); bool visible = true;
            while (item && item->parent) { item = explorer_.item(item->parent); if (item && !item->expanded) { visible = false; break; } }
            if (visible) requestDirectory(pair.second);
        }
    }
    refreshGit(files);
    const bool showingGitChanges = std::any_of(tabs_.begin(), tabs_.end(), [](const Tab& tab) { return tab.displayed && tab.diff && tab.diffKind != GitDiffKind::Commit; });
    if (!tabs_.empty() && (force || fileDirty_ || showingGitChanges)) {
        if (!IsWindowVisible(window_) && !viewer().hasDocument()) { fileDirty_ = true; return; }
        saveTab(); loadTab(true, 0); loadTab(true, 1);
    }
    fileDirty_ = false;
}
void ProjectRuntime::Impl::watch() {
    if (root_.wsl()) return;
    watchDirectory_.reset(CreateFileW(root_.local().c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
    watchEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!watchDirectory_ || !watchEvent_) { status(L"Folder watching unavailable; use F5 to refresh."); stopWatch(); return; }
    watchOperation_ = {}; watchOperation_.hEvent = watchEvent_.value; armWatch();
}
void ProjectRuntime::Impl::armWatch() {
    if (!watchDirectory_) return;
    ResetEvent(watchEvent_.value);
    watchPending_ = ReadDirectoryChangesW(watchDirectory_.value, watchBuffer_.data(), static_cast<DWORD>(watchBuffer_.size()), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, nullptr, &watchOperation_, nullptr) != FALSE;
    if (!watchPending_) status(L"Folder watching unavailable; use F5 to refresh.");
}
void ProjectRuntime::Impl::pollWatch() {
    const auto now = GetTickCount64();
    if (root_.wsl() && provider_) {
        const auto usage = provider_->watchUsage();
        if (!(usage == watchUsage_)) { watchUsage_ = usage; statusDirty_ = true; }
        const auto changes = connected_ ? provider_->takeChanges() : ProviderChanges{};
        if (connected_ && provider_->state() != ConnectionState::Ready) { connected_ = false; ++generation_; ++serial_[0]; ++serial_[1]; connectionError_ = provider_->error(); status(L"WSL disconnected. " + connectionError_ + L" Your edits have been kept. Use F5 to reconnect."); if (owner_ && owner_->changed) owner_->changed(); }
        else if (connected_) {
            if (changes.watchLimited) { watchLimited_ = true; status(L"Some WSL changes cannot be watched automatically. Use F5 to refresh."); }
            if (changes.changed) {
                if (!dirty_) dirtySince_ = now;
                dirty_ = true; filesDirty_ |= changes.files; lastEvent_ = now;
                fileDirty_ |= changes.allContent;
                for (const auto& path : changes.contentPaths) for (const auto& tab : tabs_)
                    if (tab.displayed && path.contains(tab.path)) fileDirty_ = true;
            }
        }
    }
    if (watchPending_ && WaitForSingleObject(watchEvent_.value, 0) == WAIT_OBJECT_0) {
        DWORD bytes = 0; const bool ok = GetOverlappedResult(watchDirectory_.value, &watchOperation_, &bytes, FALSE) != FALSE;
        bool relevant = !ok || !bytes;
        if (relevant) fileDirty_ = filesDirty_ = true;
        for (size_t offset = 0; ok && offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytes;) {
            const auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(watchBuffer_.data() + offset);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const auto change = classifyFileChange(utf8(name), info->Action != FILE_ACTION_MODIFIED, true);
            relevant |= change.relevant; filesDirty_ |= change.files;
            if (change.content && !tabs_.empty()) {
                const auto changed = pathKey(root_ / name), selected = pathKey(selectedPath());
                if (changed == selected || selected.rfind(changed + L"\\", 0) == 0 || (groupTab(1 - focusedGroup_) < tabs_.size() && changed == pathKey(tabs_[groupTab(1 - focusedGroup_)].path))) fileDirty_ = true;
            }
            if (!info->NextEntryOffset) break;
            offset += info->NextEntryOffset;
        }
        if (relevant) { if (!dirty_) dirtySince_ = now; dirty_ = true; lastEvent_ = now; }
        watchPending_ = false; armWatch();
    }
    if (dirty_ && (now - lastEvent_ >= 500 || now - dirtySince_ >= 2000)) { dirty_ = false; refresh(false); }
    if (statusDirty_) { statusDirty_ = false; updateStatusBar(); }
}
void ProjectRuntime::Impl::stopWatch() {
    KillTimer(window_, timerId);
    if (watchDirectory_ && watchPending_) { CancelIoEx(watchDirectory_.value, &watchOperation_); DWORD bytes = 0; GetOverlappedResult(watchDirectory_.value, &watchOperation_, &bytes, TRUE); }
    watchDirectory_.reset(); watchEvent_.reset(); watchPending_ = dirty_ = fileDirty_ = filesDirty_ = false;
}
void ProjectRuntime::Impl::toggleTerminal() {
    terminalVisible_ = !terminalVisible_; layout(); SetFocus(terminalVisible_ ? terminal_.window() : viewer().window());
}
LRESULT CALLBACK ProjectRuntime::Impl::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<ProjectRuntime::Impl*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<ProjectRuntime::Impl*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT ProjectRuntime::Impl::handle(UINT m, WPARAM a, LPARAM b) {
    switch (m) {
    case WM_CREATE: create(); return 0;
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: { auto info = reinterpret_cast<MINMAXINFO*>(b); info->ptMinTrackSize = {MulDiv(720, dpi_, 96), MulDiv(480, dpi_, 96)}; return 0; }
    case WM_DPICHANGED: { dpi(HIWORD(a)); const auto r = reinterpret_cast<RECT*>(b); SetWindowPos(window_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case wmWork: worker_->dispatch(); gitWorker_->dispatch(); return 0;
    case WM_TIMER: if (a == timerId) pollWatch(); else if (a == tabDragTimerId) scrollTabDrag(); return 0;
    case WM_APP + 14: loadTab(false, 0); loadTab(false, 1); return 0;
    case WM_APP + 13: if (!root_.empty()) { if (!watchDirectory_) watch(); refresh(); } return 0;
    case WM_APP + 12:
        if (!root_.empty() && IsWindowVisible(window_)) {
            pollWatch();
            if (!root_.wsl() || connected_) {
                for (int group = 0; group < 2; ++group) if (groupTab(group) < tabs_.size() && !viewers_[group].hasDocument()) loadTab(true, group);
                if (watchLimited_) status(L"Some WSL changes cannot be watched automatically. Use F5 to refresh.");
            }
        }
        return 0;
    case WM_LBUTTONDOWN: {
        quickOpen_.close();
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        if (activeSidebar_ == Sidebar::Git) for (const bool tree : {true, false}) {
            const auto button = gitViewButton(tree);
            if (PtInRect(&button, point)) { setGitView(tree); return 0; }
        }
        if (PtInRect(&tabsRect_, point)) {
            const auto index = tabAt(point);
            if (index < tabs_.size()) {
                const auto rect = tabBounds(index);
                if (!tabs_[index].pinned && point.x >= rect.right - MulDiv(28, dpi_, 96)) closeTab(index);
                else {
                    selectTab(index); SetFocus(viewer().window());
                    tabDrag_ = TabDrag{index, point, point, false, std::nullopt};
                    SetCapture(window_);
                }
            } else for (auto& pane : editorGroups_) if (pane.overflow() && PtInRect(&pane.tabs, point) && point.y >= pane.tabs.bottom - MulDiv(34, dpi_, 96)) {
                const int button = MulDiv(28, dpi_, 96);
                if (point.x >= pane.tabs.right - button && pane.firstRow + pane.rows < pane.starts.size()) ++pane.firstRow;
                else if (point.x >= pane.tabs.right - 2 * button && point.x < pane.tabs.right - button && pane.firstRow) --pane.firstRow;
                layout(false);
            }
            return 0;
        }
        if (PtInRect(&sidebarSplitter_, point)) { dragging_ = 1; dragOffset_ = point.x - sidebarSplitter_.left; }
        else if (terminalVisible_ && PtInRect(&terminalSplitter_, point)) dragging_ = 2;
        else if (PtInRect(&editorSplitter_, point)) dragging_ = 3;
        if (dragging_) SetCapture(window_);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        const auto index = tabAt(point);
        if (index < tabs_.size()) {
            const auto rect = tabBounds(index);
            if (!tabs_[index].pinned && point.x >= rect.right - MulDiv(28, dpi_, 96)) return 0;
            selectTab(index); tabs_[index].preview = false; updateTabs(); SetFocus(viewer().window());
            if (owner_ && owner_->changed) owner_->changed();
        }
        return 0;
    }
    case WM_MBUTTONDOWN: { closeTab(tabAt({GET_X_LPARAM(b), GET_Y_LPARAM(b)})); return 0; }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(a) == window_) { tabMenu({GET_X_LPARAM(b), GET_Y_LPARAM(b)}); return 0; }
        break;
    case WM_MOUSEMOVE:
        if (tabDrag_) { updateTabDrag({GET_X_LPARAM(b), GET_Y_LPARAM(b)}); return 0; }
        if (dragging_) {
            RECT r{}; GetClientRect(window_, &r);
            if (dragging_ == 1) sidebar_ = std::clamp(MulDiv(GET_X_LPARAM(b) - activityWidth_ - dragOffset_, 96, dpi_), 130, std::max(130, MulDiv(r.right - activityWidth_ - splitter_, 96, dpi_) - 250));
            else if (dragging_ == 3) editorRatio_ = std::clamp(MulDiv(GET_X_LPARAM(b) - editorRect_.left, 1000, std::max(1L, editorRect_.right - editorRect_.left - splitter_)), 200, 800);
            else terminalHeight_ = std::clamp(MulDiv(r.bottom - footer_ - GET_Y_LPARAM(b), 96, dpi_), 100, std::max(100, MulDiv(r.bottom - footer_, 96, dpi_) - 120));
            layout();
        } return 0;
    case WM_LBUTTONUP:
        if (tabDrag_ && tabDrag_->active) updateTabDrag({GET_X_LPARAM(b), GET_Y_LPARAM(b)});
        endTabDrag(true);
        dragging_ = 0; if (GetCapture() == window_) ReleaseCapture(); return 0;
    case WM_CANCELMODE:
        endTabDrag(); dragging_ = 0; if (GetCapture() == window_) ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: endTabDrag(); dragging_ = 0; return 0;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(a) == window_ && LOWORD(b) == HTCLIENT) {
            if (tabDrag_) return TRUE;
            POINT p{}; GetCursorPos(&p); ScreenToClient(window_, &p);
            if (PtInRect(&sidebarSplitter_, p) || PtInRect(&editorSplitter_, p)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            if (terminalVisible_ && PtInRect(&terminalSplitter_, p)) { SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
        }
        break;
    case WM_NOTIFY: {
        const auto header = reinterpret_cast<NMHDR*>(b);
        if (header->hwndFrom == tooltips_ && header->code == TTN_GETDISPINFOW && header->idFrom >= 100 && header->idFrom - 100 < tabTools_) {
            const size_t index = visibleTabs_[header->idFrom - 100];
            if (index < tabs_.size()) { tabTip_ = tabs_[index].path.native(); reinterpret_cast<NMTTDISPINFOW*>(b)->lpszText = tabTip_.data(); }
            return 0;
        }
        return 0;
    }
    case WM_CLOSE: return 0;
    case WM_DESTROY:
        KillTimer(window_, timerId); stopWatch(); quickOpen_.stop(); searchPane_.stop(); if (gitWorker_) gitWorker_->stop(); stopExplorerHistory(); if (worker_) worker_->stop(); terminal_.stop(); explorer_.clear(); changes_.clear();
        return 0;
    default: break;
    }
    return DefWindowProcW(window_, m, a, b);
}
bool ProjectRuntime::Impl::key(const MSG& message) {
        if (terminal_.key(message)) return true;
        // Let every terminal session (and its rename input) receive its own keys.
        if (terminal_.contains(message.hwnd)) return false;
        if (gitCommit_.contains(message.hwnd)) return gitCommit_.key(message);
        if (explorer_.dragging() && (message.message == WM_KEYDOWN || message.message == WM_CHAR)) {
            SendMessageW(tree_, message.message, message.wParam, message.lParam); return true;
        }
        const bool key = message.message == WM_KEYDOWN;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (key && message.wParam == VK_ESCAPE && tabDrag_) { endTabDrag(); return true; }
        if (key && ctrl && !shift && !(GetKeyState(VK_MENU) & 0x8000) && message.wParam == 'N') { newFile(); return true; }
        if (key && ctrl && !shift && !(GetKeyState(VK_MENU) & 0x8000) && message.wParam == 'J') { quickOpen_.close(); toggleTerminal(); return true; }
        if (key && ctrl && shift && !(GetKeyState(VK_MENU) & 0x8000) && message.wParam == 'F') { quickOpen_.close(); selectSidebar(Sidebar::Search); return true; }
        if (key && ctrl && !shift && !(GetKeyState(VK_MENU) & 0x8000) && message.wParam == 'P') { quickOpen_.show(); return true; }
        if (key && ctrl && message.wParam == 'O') { quickOpen_.close(); chooseFolder(); return true; }
        if (quickOpen_.visible() && key) return false;
        const bool searchFocus = message.hwnd == searchPane_.window() || IsChild(searchPane_.window(), message.hwnd);
        if (searchFocus && key && !ctrl && message.wParam == VK_F5) {
            if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) connectRemote(); else searchPane_.refresh();
            return true;
        }
        if (!searchFocus && GetFocus() != terminal_.window() && viewer().findKey(message)) return true;
        if (searchFocus && key && ctrl && !shift && message.wParam == 'F') { viewer().find(); return true; }
        if (searchFocus && key && !ctrl && message.wParam == VK_TAB) return false;
        if (key && !ctrl && IsChild(viewer().window(), message.hwnd) && message.wParam == VK_TAB) return false;
        if (key && ctrl && shift && message.wParam == 'E') { selectSidebar(Sidebar::Explorer); return true; }
        if (key && ctrl && shift && message.wParam == 'G') { selectSidebar(Sidebar::Git); return true; }
        if (key && ctrl && shift && message.wParam == 'B') { selectSidebar(Sidebar::Git); return true; }
        if (key && ctrl && message.wParam == VK_OEM_3) { toggleTerminal(); return true; }
        if (key && ctrl && message.wParam == VK_TAB && !tabs_.empty()) { selectTab((activeTab_ + (shift ? tabs_.size() - 1 : 1)) % tabs_.size()); SetFocus(viewer().window()); return true; }
        if (key && ctrl && message.wParam == 'W' && GetFocus() != terminal_.window()) { closeTab(activeTab_); return true; }
        if (key && message.wParam == VK_F5 && GetFocus() != terminal_.window()) { refresh(); return true; }
        if (key && message.wParam == VK_F6 && GetFocus() != terminal_.window()) { showDiff(); return true; }
        if (key && message.wParam == VK_F7 && GetFocus() != terminal_.window()) { const bool inlineView = !viewer().diffInline(); for (auto& view : viewers_) view.setDiffLayout(inlineView); return true; }
        if (key && message.wParam == VK_TAB && GetFocus() != terminal_.window() && GetFocus() != viewers_[0].window() && GetFocus() != viewers_[1].window()) {
            const HWND files = activeSidebar_ == Sidebar::Search ? searchPane_.window() : activeSidebar_ == Sidebar::Explorer ? (IsWindowVisible(tree_) ? tree_ : explorerButton_) : (IsWindowVisible(gitTree_) ? gitTree_ : gitButton_);
            SetFocus(GetFocus() == files ? viewer().window() : terminalVisible_ ? terminal_.window() : files); return true;
        }

    return false;
}
ProjectRuntime::ProjectRuntime() : impl_(std::make_unique<Impl>()) { impl_->owner_ = this; }
ProjectRuntime::~ProjectRuntime() {
    shutdown();
    if (impl_->window_ && IsWindow(impl_->window_)) DestroyWindow(impl_->window_);
}
void ProjectRuntime::create(HWND parent, HINSTANCE instance, AiEventQueue* hooks) {
    impl_->instance_ = instance;
    WNDCLASSW cls{}; cls.style = CS_DBLCLKS; cls.lpfnWndProc = Impl::procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyProject"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    impl_->window_ = CreateWindowExW(0, cls.lpszClassName, L"Project", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr, instance, impl_.get());
    impl_->terminal_.connect(hooks);
}
void ProjectRuntime::shutdown() {
    impl_->stopWatch(); impl_->quickOpen_.stop(); impl_->searchPane_.stop();
    if (impl_->gitWorker_) impl_->gitWorker_->stop();
    impl_->stopExplorerHistory();
    if (impl_->worker_) impl_->worker_->stop();
    impl_->terminal_.stop();
}
bool ProjectRuntime::canClose() {
    if (impl_->gitDiscardPending_ || impl_->gitStashPending_ || impl_->gitCommitPending_) { impl_->gitWorker_->drain(); impl_->gitWorker_->dispatch(); if (impl_->gitDiscardPending_ || impl_->gitStashPending_ || impl_->gitCommitPending_) return false; }
    if (impl_->renamePending_ || impl_->pastePending_ || impl_->createPending_) { impl_->worker_->drain(); impl_->worker_->dispatch(); }
    impl_->saveTab(); std::set<const EditBuffer*> confirmed;
    for (size_t i = 0; i < impl_->tabs_.size(); ++i)
        if ((!impl_->tabs_[i].buffer || confirmed.insert(impl_->tabs_[i].buffer.get()).second) && !impl_->confirmTab(i)) return false;
    return true;
}
void ProjectRuntime::revalidate() { PostMessageW(window(), WM_APP+13, 0, 0); }
void ProjectRuntime::initialize(const ResourceId& path, const ProjectSession& state, bool saved) { if (saved) impl_->initialState_ = state; impl_->openFolder(path); }
void ProjectRuntime::activate(bool active) {
    impl_->active_ = active;
    impl_->searchPane_.active(active && impl_->activeSidebar_ == Sidebar::Search);
    if (active && impl_->root_.wsl() && !impl_->connected_ && impl_->provider_ && impl_->provider_->state() == ConnectionState::Disconnected) impl_->connectRemote();
    if (!active) {
        impl_->saveTab(); impl_->lastFocus_ = GetFocus();
        if (impl_->lastFocus_ == impl_->terminal_.window()) impl_->focusedPane_ = "terminal";
        else if (impl_->lastFocus_ == impl_->tree_) impl_->focusedPane_ = "explorer";
        else if (impl_->lastFocus_ == impl_->gitTree_) impl_->focusedPane_ = "git";
        else if (impl_->lastFocus_ == impl_->viewer().window() || IsChild(impl_->viewer().window(), impl_->lastFocus_)) impl_->focusedPane_ = "viewer";
        impl_->quickOpen_.close(false); impl_->endTabDrag(); ShowWindow(window(), SW_HIDE);
    }
    else {
        ShowWindow(window(), SW_SHOWNA); impl_->layout(false);
        HWND focus = impl_->lastFocus_;
        if (!focus || !IsWindow(focus) || !IsWindowVisible(focus)) {
            focus = impl_->focusedPane_ == "terminal" && impl_->terminalVisible_ ? impl_->terminal_.window() : impl_->focusedPane_ == "explorer" && IsWindowVisible(impl_->tree_) ? impl_->tree_ : impl_->focusedPane_ == "git" && IsWindowVisible(impl_->gitTree_) ? impl_->gitTree_ : impl_->viewer().window();
        }
        SetFocus(focus); impl_->terminal_.acknowledge(); PostMessageW(window(), WM_APP+12, 0, 0);
    }
}
void ProjectRuntime::setDpi(UINT dpi) { impl_->dpi(dpi); }
void ProjectRuntime::poll() { if (ready()) impl_->pollWatch(); if (IsWindowVisible(impl_->terminal_.container())) { impl_->terminal_.acknowledge(); if (summary().counts[static_cast<size_t>(AiState::Running)]) InvalidateRect(impl_->terminal_.container(), nullptr, FALSE); } }
bool ProjectRuntime::key(const MSG& message) { return impl_->key(message); }
bool ProjectRuntime::terminalInput(HWND window) const { return impl_->terminal_.contains(window); }
ProjectSession ProjectRuntime::snapshot() { return impl_->saveSession(); }
AiSummary ProjectRuntime::summary() const { return impl_->terminal_.summary(); }
std::wstring ProjectRuntime::description() const { return (impl_->root_.wsl() ? L"\n" + (impl_->connectionError_.empty() ? impl_->provider_->error() : impl_->connectionError_) + (impl_->watchLimited_ ? L"\nSome changes cannot be watched automatically. Use F5 to refresh." : L"") : L"") + impl_->terminal_.description(); }
bool ProjectRuntime::apply(const AiEvent& event) { return impl_->terminal_.apply(event); }
void ProjectRuntime::focusAttention() { impl_->terminalVisible_ = true; impl_->layout(false); impl_->terminal_.focusAttention(); }
size_t ProjectRuntime::documentBytes() const {
    if (std::any_of(impl_->tabs_.begin(), impl_->tabs_.end(), [](const Tab& tab) { return tab.buffer && (tab.buffer->dirty() || tab.buffer->saving); })) return 0;
    size_t bytes = 0; for (const auto& view : impl_->viewers_) bytes += view.documentBytes();
    for (const auto& tab : impl_->tabs_) if (tab.buffer && tab.buffer != impl_->viewers_[0].buffer() && tab.buffer != impl_->viewers_[1].buffer()) bytes += tab.buffer->document.text.capacity() + tab.buffer->baseline.capacity();
    return bytes;
}
void ProjectRuntime::releaseDocument() {
    impl_->saveTab(); if (std::any_of(impl_->tabs_.begin(), impl_->tabs_.end(), [](const Tab& tab) { return tab.buffer && (tab.buffer->dirty() || tab.buffer->saving); })) return;
    for (auto& tab : impl_->tabs_) { tab.buffer.reset(); tab.diffBefore.reset(); }
    for (int group = 0; group < 2; ++group) { ++impl_->serial_[group]; impl_->waitingForGit_[group] = false; impl_->viewers_[group].message(L""); }
}
HWND ProjectRuntime::window() const { return impl_->window_; }
bool ProjectRuntime::ready() const { return !impl_->root_.empty(); }
bool ProjectRuntime::hasUnsaved() const { return impl_->renamePending_ || impl_->pastePending_ || impl_->createPending_ || std::any_of(impl_->tabs_.begin(), impl_->tabs_.end(), [](const Tab& tab) { return tab.buffer && (tab.buffer->dirty() || tab.buffer->saving); }); }
std::wstring ProjectRuntime::connectionDescription() const {
    if (!impl_->root_.wsl()) return {};
    return impl_->connecting_ ? L"Connecting" : impl_->connected_ && impl_->provider_->state() == ConnectionState::Ready ? L"Connected" : impl_->connectionError_.empty() && impl_->provider_->error().empty() ? L"Disconnected" : L"Unavailable";
}

}
