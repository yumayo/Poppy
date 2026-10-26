#include "viewer.h"
#include "terminal_pane.h"
#include "project_runtime.h"
#include "tree.h"
#include "git_changes.h"
#include "git_history.h"
#include "activity_button.h"
#include "quick_open.h"
#include "file_name.h"
#include "project_provider.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
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
constexpr int explorerCommand = 2, gitCommand = 3, branchCommand = 5;
enum class Sidebar { Explorer, Git, Branch };
using Entry = ProviderEntry;
struct Node {
    ResourceId path; uint64_t id;
    bool directory, loaded = false, loading = false, reparse = false;
    // Keep expansion intent while loading, even when an empty item cannot expand.
    bool expandRequested = false;
    bool unsupported = false; std::wstring label;
};
using DirectoryResult = ProviderListing;
struct FileResult { std::shared_ptr<EditBuffer> buffer; std::shared_ptr<Document> document; std::wstring error; };
struct Tab {
    ResourceId path; bool diff = false; Viewer::State fileView, diffView; bool pinned = false;
    std::shared_ptr<EditBuffer> buffer; int group = 0; bool displayed = false;
    GitDiffKind diffKind = GitDiffKind::Automatic; std::string revision; bool allChanges = false;
};
std::wstring tabKey(const Tab& tab) {
    return pathKey(tab.path) + L"\n" + std::to_wstring(static_cast<int>(tab.diffKind)) + L"\n" + wide(tab.revision) + (tab.allChanges ? L"\nall" : L"");
}
std::wstring tabTitle(const Tab& tab) {
    auto title = tab.allChanges ? L"All Changes" : tab.path.filename().native();
    if (tab.diffKind == GitDiffKind::Commit) title += L" · " + wide(tab.revision.substr(0, 8));
    else if (tab.diffKind == GitDiffKind::Staged) title += L" · Staged";
    else if (tab.diffKind != GitDiffKind::Automatic) title += L" · Changes";
    return title + (tab.buffer && tab.buffer->dirty() ? L" *" : L"") + (tab.diff ? L" (diff)" : L"");
}
struct TabDrop { size_t position; RECT marker; int group = 0; };
struct EditorGroup { RECT tabs{}, body{}; std::vector<size_t> indices, starts; size_t firstRow = 0, rows = 0; bool overflow() const { return starts.size() > rows; } };
struct TabDrag {
    size_t index;
    POINT origin, point;
    bool active = false;
    std::optional<TabDrop> target;
};
struct GitEntry { ResourceId path; wchar_t state; bool staged = false, changed = false; char index = ' ', worktree = ' '; };
struct GitResult { ResourceId root; std::wstring branch, error; std::map<std::wstring, wchar_t> states; std::vector<GitEntry> entries; size_t changed = 0, untracked = 0; bool truncated = false, unsupportedNames = false; };
struct SourceNode { std::wstring path; size_t entry = noGitChange; bool directory = false, staged = false, group = false; };
enum class HistoryKind { Branch, Commit, Directory, File, More, Message };
struct HistoryNode {
    HistoryKind kind; std::string ref, revision; ResourceId path;
    bool loaded = false, loading = false; size_t count = 0;
    uint64_t generation = 0;
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
    auto bytes = std::move(file.bytes); auto buffer = std::make_shared<EditBuffer>(); auto document = std::shared_ptr<Document>(buffer, &buffer->document); std::string error;
    if (!decodeText(bytes, document->text, document->encoding, error)) { result->error = wide(error); return result; }
    if (static_cast<size_t>(std::count(document->text.begin(), document->text.end(), '\n')) >= maxLines) { result->error = L"This file exceeds the 1,000,000-line preview limit."; return result; }
    document->language = path.filename() == L"CMakeLists.txt" ? Language::Config : languageFor(utf8(path.extension().native()));
    indexDocument(*document); buffer->initialize(std::move(bytes)); buffer->fileVersion = std::move(file.version); result->buffer = std::move(buffer); result->document = std::move(document); return result;
}
std::wstring trimmed(std::string text) { while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back(); return wide(text); }
std::shared_ptr<GitResult> readGit(ProjectProvider& provider, const ResourceId& directory, const std::atomic<bool>* cancelled) {
    auto result = std::make_shared<GitResult>();
    auto root = provider.git({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--show-toplevel"}, directory, cancelled);
    if (root.code) { if (root.code != 128 || root.output.find("not a git repository") == std::string::npos) result->error = root.error; return result; }
    if (directory.wsl() && !validUtf8(root.output)) { result->error = L"The Git repository root is not a UTF-8 path."; return result; }
    result->root = directory.at(utf8(trimmed(root.output)));
    auto branch = provider.git({L"git.exe", L"--no-optional-locks", L"symbolic-ref", L"--short", L"-q", L"HEAD"}, result->root, cancelled);
    if (branch.code == 0) result->branch = trimmed(branch.output);
    else {
        auto head = provider.git({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--short", L"HEAD"}, result->root, cancelled);
        result->branch = head.code == 0 ? trimmed(head.output) + L" (detached)" : L"HEAD";
    }
    auto status = provider.git({L"git.exe", L"--no-optional-locks", L"-c", L"core.fsmonitor=false", L"status", L"--porcelain=v1", L"-z", L"--untracked-files=all"}, result->root, cancelled);
    if (status.code) { result->error = status.error; return result; }
    for (const auto& entry : parseGitStatus(status.output)) {
        const char x = entry.index, y = entry.worktree;
        if (directory.wsl() && !validUtf8(entry.path)) { result->unsupportedNames = true; continue; }
        const wchar_t state = x == '?' ? L'?' : x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D') ? L'U' : y != ' ' ? y : x;
        if (state == L'?') ++result->untracked; else ++result->changed;
        ResourceId path = (result->root / std::string_view(entry.path)).lexically_normal();
        if (result->entries.size() < maxTreeNodes) result->entries.push_back({path, state, entry.staged(), entry.changed(), x, y});
        else result->truncated = true;
        auto mark = [&](ResourceId current) {
            for (;;) {
                const auto key = pathKey(current); auto found = result->states.find(key);
                if (found != result->states.end()) { if (state == L'U') found->second = state; }
                else if (result->states.size() < maxTreeNodes) result->states[key] = state;
                else result->truncated = true;
                if (current == result->root || current == current.root_path() || current.empty()) break;
                current = current.parent_path();
            }
        };
        mark(path);
        if (!entry.previousPath.empty()) { if (!directory.wsl() || validUtf8(entry.previousPath)) mark(result->root / std::string_view(entry.previousPath)); else result->unsupportedNames = true; }
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
    void openGitFile(TreeItem item);
    void openGitDiff(const ResourceId& path, GitDiffKind kind, std::string revision = {}, bool allChanges = false);
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
    TreeItem insert(TreeItem parent, const Entry& entry);
    Node* node(TreeItem item) const;
    void clearChildren(TreeItem parent);
    void updateLabel(TreeItem item);
    void renameFile(TreeItem item, const std::wstring& name);
    wchar_t stateFor(const ResourceId& path) const;
    void openFile(const ResourceId& path, bool diff = false);
    void openWithDefaultApp(ResourceId path);
    void saveTab();
    bool saveFile(size_t index, bool wait = false);
    bool confirmTab(size_t index);
    void selectTab(size_t index);
    void closeTab(size_t index);
    void toggleTabPin(size_t index);
    void tabMenu(POINT screen);
    void loadTab(bool preserve = false, int group = -1);
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
    void refreshGit();
    void watch();
    void armWatch();
    void pollWatch();
    void stopWatch();
    void status(std::wstring message);
    void toggleTerminal();
    HWND window_ = nullptr, tree_ = nullptr, gitTree_ = nullptr, branchTree_ = nullptr, explorerButton_ = nullptr, gitButton_ = nullptr, branchButton_ = nullptr, tooltips_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Tree explorer_, changes_, branches_;
    Renderer renderer_;
    ActivityButton explorerActivity_, gitActivity_, branchActivity_;
    UINT dpi_ = 96;
    int sidebar_ = 245, terminalHeight_ = 260, toolbar_ = 44, footer_ = 26, splitter_ = 5;
    int activityWidth_ = 48, sidebarHeader_ = 34, gitHeader_ = 84;
    RECT sidebarRect_{}, sidebarSplitter_{}, terminalSplitter_{}, tabsRect_{};
    int dragging_ = 0, dragOffset_ = 0;
    bool terminalVisible_ = true;
    bool updatingGitFiles_ = false;
    bool renamePending_ = false;
    bool gitTreeView_ = true;
    std::vector<SourceNode> gitChangeNodes_;
    uint64_t branchRequest_ = 0;
    bool branchesLoading_ = false, branchesLoaded_ = false;
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
    bool waitingForGit_[2]{};
    std::vector<Tab> tabs_;
    std::vector<RECT> tabRects_;
    std::vector<size_t> visibleTabs_;
    size_t activeTab_ = 0, tabTools_ = 0;

    std::optional<TabDrag> tabDrag_;
    TreeItem rootItem_ = 0;
    uint64_t generation_ = 0, serial_[2]{}, nextId_ = 1;
    std::map<uint64_t, TreeItem> nodes_;
    std::shared_ptr<GitResult> git_;
    std::shared_ptr<ProjectProvider> provider_;
    bool connecting_ = false, connected_ = false, active_ = false;
    uint64_t providerRevision_ = 0;
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
    changes_.create(window_, instance_, L"Git changes", 4); gitTree_ = changes_.window();
    branches_.create(window_, instance_, L"Branches", 6); branchTree_ = branches_.window();
    branches_.hoverActionsOnly = true;
    branches_.removed = [](uintptr_t data) { delete reinterpret_cast<HistoryNode*>(data); };
    branches_.expanded = [this](TreeItem item, bool expanded) { if (expanded) requestHistory(item); };
    branches_.activated = [this](TreeItem item) { openHistory(item); };
    branches_.actionLabel = [this](TreeItem item) { const auto n = historyNode(item); return n && n->kind == HistoryKind::Commit ? L"Open All Changes" : L""; };
    branches_.actionClicked = [this](TreeItem item) { const auto n = historyNode(item); if (n && n->kind == HistoryKind::Commit && git_) openGitDiff(git_->root, GitDiffKind::Commit, n->revision, true); };
    explorer_.activated = [this](TreeItem item) {
        pendingTreeSelection_.clear();
        if (const auto n = node(item); n && !n->directory) openFile(n->path);
    };
    explorer_.expanded = [this](TreeItem item, bool value) {
        const auto n = node(item); if (!n) return;
        if (value) { expandedPaths_[pathKey(n->path)] = n->path; if (!n->loaded) requestDirectory(item); }
        else { forgetExpanded(n->path); n->expandRequested = false; clearChildren(item); n->loaded = false; }
    };
    explorer_.removed = [this](uintptr_t data) { const auto n = reinterpret_cast<Node*>(data); if (n) { nodes_.erase(n->id); delete n; } };
    explorer_.canRename = [this](TreeItem item) { return item != rootItem_ && node(item) && !node(item)->unsupported && !renamePending_ && (!root_.wsl() || (connected_ && provider_->state() == ConnectionState::Ready)); };
    explorer_.renameRequested = [this](TreeItem item) { if (const auto n = node(item); n && item != rootItem_ && !renamePending_) explorer_.rename(item, n->path.filename().native()); };
    explorer_.renameCommitted = [this](TreeItem item, const std::wstring& name) { renameFile(item, name); };
    explorer_.canOpenWithDefaultApp = [this](TreeItem item) { const auto n = node(item); return n && !n->directory && !n->unsupported && !root_.wsl(); };
    explorer_.openWithDefaultAppRequested = [this](TreeItem item) { if (const auto n = node(item); n && !n->directory) openWithDefaultApp(n->path); };
    changes_.activated = [this](TreeItem item) { openGitFile(item); };
    changes_.actionLabel = [this](TreeItem item) {
        const auto n = changes_.item(item);
        return n && n->data < gitChangeNodes_.size() && gitChangeNodes_[n->data].group && changes_.child(item) ? L"Open All Changes" : L"";
    };
    changes_.actionClicked = [this](TreeItem item) {
        const auto n = changes_.item(item);
        if (n && n->data < gitChangeNodes_.size() && gitChangeNodes_[n->data].group && git_)
            openGitDiff(git_->root, gitChangeNodes_[n->data].staged ? GitDiffKind::Staged : GitDiffKind::Changes, {}, true);
    };
    changes_.expanded = [this](TreeItem item, bool value) {
        if (updatingGitFiles_) return;
        const auto node = changes_.item(item);
        if (!node || node->data >= gitChangeNodes_.size()) return;
        const auto& path = gitChangeNodes_[node->data].path;
        if (value) collapsedGitPaths_.erase(path);
        else if (collapsedGitPaths_.size() < maxTreeNodes) collapsedGitPaths_.insert(path);
    };
    explorerActivity_.create(window_, instance_, L"Explorer", explorerCommand, false); explorerButton_ = explorerActivity_.window();
    gitActivity_.create(window_, instance_, L"Source Control", gitCommand, true); gitButton_ = gitActivity_.window();
    branchActivity_.create(window_, instance_, L"Branch", branchCommand, false, true); branchButton_ = branchActivity_.window();
    explorerActivity_.clicked = [this] { selectSidebar(Sidebar::Explorer); };
    gitActivity_.clicked = [this] { selectSidebar(Sidebar::Git); };
    branchActivity_.clicked = [this] { selectSidebar(Sidebar::Branch); };
    explorerActivity_.select(true);
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    for (const auto button : {explorerButton_, gitButton_, branchButton_}) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tip.hwnd = window_; tip.uId = reinterpret_cast<UINT_PTR>(button);
        tip.lpszText = const_cast<wchar_t*>(button == explorerButton_ ? L"Explorer (Ctrl+Shift+E)" : button == gitButton_ ? L"Source Control (Ctrl+Shift+G)" : L"Branch (Ctrl+Shift+B)");
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    for (int group = 0; group < 2; ++group) {
        auto& view = viewers_[group]; view.create(window_, instance_);
        view.changed = [this] { saveTab(); updateTabs(false); if (owner_ && owner_->changed) owner_->changed(); };
        view.save = [this, group] { saveFile(groupTab(group)); };
        view.diffLayoutChanged = [this, group] { viewers_[1 - group].setDiffLayout(viewers_[group].diffInline()); };
        view.focused = [this, group] { focusedGroup_ = group; if (groupTab(group) < tabs_.size()) activeTab_ = groupTab(group); updateTabs(false); };
    }
    terminal_.create(window_, instance_);
    dropPreview_.create(window_, instance_);
    terminal_.changed = [this] { if (owner_ && owner_->changed) owner_->changed(); };
    quickOpen_.create(window_, instance_);
    quickOpen_.opened = [this](const ResourceId& path) { openFile(path); SetFocus(viewer().window()); };
    // Each underlying pane must clip the palette when repainting beneath it.
    for (const auto pane : {tree_, gitTree_, branchTree_, viewers_[0].window(), viewers_[1].window(), terminal_.container(), explorerButton_, gitButton_, branchButton_})
        SetWindowLongPtrW(pane, GWL_STYLE, GetWindowLongPtrW(pane, GWL_STYLE) | WS_CLIPSIBLINGS);
    const BOOL dark = TRUE; DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    dpi(GetDpiForWindow(window_));
}
void ProjectRuntime::Impl::dpi(UINT value) {
    dpi_ = value; renderer_.setDpi(dpi_);
    explorer_.setDpi(dpi_); changes_.setDpi(dpi_, 28); branches_.setDpi(dpi_, 28);
    explorerActivity_.setDpi(dpi_); gitActivity_.setDpi(dpi_); branchActivity_.setDpi(dpi_);
    toolbar_ = MulDiv(44, dpi_, 96); footer_ = MulDiv(26, dpi_, 96); splitter_ = MulDiv(5, dpi_, 96);
    activityWidth_ = MulDiv(48, dpi_, 96); sidebarHeader_ = MulDiv(34, dpi_, 96); gitHeader_ = MulDiv(84, dpi_, 96);
    for (auto& view : viewers_) view.setDpi(dpi_);
    terminal_.setDpi(dpi_); quickOpen_.setDpi(dpi_); layout();
}
void ProjectRuntime::Impl::layout(bool revealTab) {
    if (!tree_) return;
    RECT r{}; GetClientRect(window_, &r);
    const int width = static_cast<int>(r.right), height = static_cast<int>(r.bottom);
    const int side = std::clamp(MulDiv(sidebar_, dpi_, 96), MulDiv(130, dpi_, 96), std::max(MulDiv(130, dpi_, 96), width - activityWidth_ - splitter_ - MulDiv(250, dpi_, 96)));
    const int right = activityWidth_ + side + splitter_, content = std::max(0, height - toolbar_ - footer_);
    const int terminalSize = terminalVisible_ ? std::min(MulDiv(terminalHeight_, dpi_, 96), std::max(0, content - MulDiv(100, dpi_, 96))) : 0;
    const int viewerSize = std::max(0, content - terminalSize - (terminalVisible_ ? splitter_ : 0));
    sidebarRect_ = {activityWidth_, toolbar_, activityWidth_ + side, toolbar_ + content};
    sidebarSplitter_ = {sidebarRect_.right, toolbar_, right, toolbar_ + content};
    terminalSplitter_ = {right, toolbar_ + viewerSize, width, toolbar_ + viewerSize + (terminalVisible_ ? splitter_ : 0)};
    const int gitTop = toolbar_ + sidebarHeader_ + gitHeader_;
    const bool showExplorer = activeSidebar_ == Sidebar::Explorer && !root_.empty();
    const bool showGitFiles = activeSidebar_ == Sidebar::Git && git_ && git_->error.empty() && !git_->root.empty() && !git_->entries.empty();
    normalizeGroups();
    editorRect_ = {right, toolbar_, width, toolbar_ + viewerSize};
    const bool split = std::any_of(tabs_.begin(), tabs_.end(), [](const Tab& tab) { return tab.group == 1; });
    const int middle = right + MulDiv(std::max(0, width - right - splitter_), editorRatio_, 1000);
    editorSplitter_ = split ? RECT{middle, toolbar_, middle + splitter_, toolbar_ + viewerSize} : RECT{};
    const int rowHeight = MulDiv(34, dpi_, 96);
    const size_t maxRows = static_cast<size_t>(std::max(1, (viewerSize - MulDiv(100, dpi_, 96)) / rowHeight));
    tabRects_.assign(tabs_.size(), RECT{});
    for (int group = 0; group < 2; ++group) {
        auto& pane = editorGroups_[group]; pane.indices.clear();
        for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].group == group) pane.indices.push_back(i);
        const int start = group ? middle + splitter_ : right, edge = group || !split ? width : middle;
        pane.tabs = {start, toolbar_, edge, toolbar_};
        const auto wrap = [&](int available) {
            pane.starts.clear(); int x = start, row = -1;
            for (size_t j = 0; j < pane.indices.size(); ++j) {
                const size_t i = pane.indices[j]; const auto& tab = tabs_[i];
                const auto title = tabTitle(tab);
                const int width = std::min(std::max(0, available), renderer_.textWidth(title) + MulDiv(40, dpi_, 96));
                if (!j || tab.pinned != tabs_[pane.indices[j - 1]].pinned || (x > start && x + width > start + available)) { pane.starts.push_back(j); x = start; ++row; }
                tabRects_[i] = {x, toolbar_ + row * rowHeight, x + width, toolbar_ + (row + 1) * rowHeight}; x += width;
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
        pane.tabs.bottom = toolbar_ + static_cast<int>(pane.rows) * rowHeight;
        pane.body = {start, pane.tabs.bottom, edge, toolbar_ + viewerSize};
        for (const auto i : pane.indices) OffsetRect(&tabRects_[i], 0, -static_cast<int>(pane.firstRow) * rowHeight);
    }
    tabsRect_ = {right, toolbar_, width, std::max(editorGroups_[0].tabs.bottom, editorGroups_[1].tabs.bottom)};
    updateTabs(false);
    if (tabDrag_ && tabDrag_->active) tabDrag_->target = tabDropAt(tabDrag_->point);

    struct Placement { HWND window; int x, y, width, height, flags; };
    const std::array<Placement, 9> panes{{
        {explorerButton_, 0, toolbar_, activityWidth_ - 1, activityWidth_, SWP_SHOWWINDOW},
        {gitButton_, 0, toolbar_ + activityWidth_, activityWidth_ - 1, activityWidth_, SWP_SHOWWINDOW},
        {branchButton_, 0, toolbar_ + 2 * activityWidth_, activityWidth_ - 1, activityWidth_, SWP_SHOWWINDOW},
        {tree_, activityWidth_, toolbar_ + sidebarHeader_, side, std::max(0, content - sidebarHeader_), showExplorer ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {gitTree_, activityWidth_, gitTop, side, std::max(0, toolbar_ + content - gitTop), showGitFiles ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {branchTree_, activityWidth_, toolbar_ + sidebarHeader_, side, std::max(0, content - sidebarHeader_), activeSidebar_ == Sidebar::Branch && branches_.child() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {viewers_[0].window(), editorGroups_[0].body.left, editorGroups_[0].body.top, std::max(0L, editorGroups_[0].body.right - editorGroups_[0].body.left), std::max(0L, editorGroups_[0].body.bottom - editorGroups_[0].body.top), SWP_SHOWWINDOW},
        {viewers_[1].window(), editorGroups_[1].body.left, editorGroups_[1].body.top, std::max(0L, editorGroups_[1].body.right - editorGroups_[1].body.left), std::max(0L, editorGroups_[1].body.bottom - editorGroups_[1].body.top), split ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {terminal_.container(), right, toolbar_ + viewerSize + splitter_, std::max(0, width - right), terminalSize,
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
    dc.fill(r, border); dc.fill({0, 0, r.right, toolbar_}, panel); dc.fill({0, r.bottom - footer_, r.right, r.bottom}, panel);
    auto scaled = [&](int v) { return MulDiv(v, dpi_, 96); };
    dc.label({scaled(16), 0, scaled(85), toolbar_}, L"POPPY", accent);
    dc.label({scaled(98), 0, scaled(210), toolbar_}, L"Open folder", foreground);
    dc.label({scaled(220), 0, scaled(292), toolbar_}, L"Refresh", foreground);
    dc.label({scaled(305), 0, scaled(360), toolbar_}, L"Diff", foreground);
    dc.label({scaled(374), 0, scaled(460), toolbar_}, L"Terminal", terminalVisible_ ? accent : muted);
    dc.label({scaled(480), 0, scaled(566), toolbar_}, L"Find file", foreground);
    dc.label({scaled(582), 0, r.right - scaled(16), toolbar_}, root_.empty() ? L"" : root_.native(), muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    const std::wstring gitLabel = !git_ ? L"Git" : !git_->error.empty() ? L"Git unavailable" : git_->root.empty() ? L"No Git repository" : git_->branch + L"  ·  " + std::to_wstring(git_->changed) + L" changed  ·  " + std::to_wstring(git_->untracked) + L" untracked";
    dc.label({scaled(12), r.bottom - footer_, scaled(390), r.bottom}, gitLabel, muted);
    dc.label({scaled(400), r.bottom - footer_, r.right - scaled(12), r.bottom}, status_, muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    dc.fill({0, toolbar_, activityWidth_, r.bottom - footer_}, background);
    dc.fill({activityWidth_ - 1, toolbar_, activityWidth_, r.bottom - footer_}, border);
    dc.fill(sidebarRect_, panel);
    const int left = sidebarRect_.left + scaled(14), edge = sidebarRect_.right - scaled(12), top = toolbar_ + sidebarHeader_;
    dc.label({left, toolbar_, edge, top}, activeSidebar_ == Sidebar::Explorer ? L"EXPLORER" : activeSidebar_ == Sidebar::Git ? L"SOURCE CONTROL" : L"BRANCH", foreground);
    dc.fill({sidebarRect_.left, top - 1, sidebarRect_.right, top}, border);
    if (activeSidebar_ == Sidebar::Git) {
        const bool repository = git_ && !git_->root.empty() && git_->error.empty();
        dc.label({left, top, edge, top + scaled(28)}, repository ? git_->branch : L"Git", repository ? accent : muted);
        const auto summary = repository ? std::to_wstring(git_->changed + git_->untracked) + L" pending changes" : L"";
        dc.label({left, top + scaled(28), edge, top + scaled(56)}, summary, muted);
        for (const bool tree : {true, false}) {
            const auto button = gitViewButton(tree);
            dc.fill(button, tree == gitTreeView_ ? rgb(50, 70, 94) : background);
            dc.label(button, tree ? L"Tree" : L"List", tree == gitTreeView_ ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        dc.fill({sidebarRect_.left, top + gitHeader_ - 1, sidebarRect_.right, top + gitHeader_}, border);
        if (!repository || git_->entries.empty()) {
            const std::wstring message = root_.empty() ? L"Open a folder to view Git changes." : !git_ ? L"Loading Git changes…" : !git_->error.empty() ? L"Git unavailable.\n" + git_->error.substr(0, 2048) + L"\nPress F5 to retry." : git_->root.empty() ? L"This folder is not in a Git repository." : L"No pending changes.";
            dc.label({left, top + gitHeader_ + scaled(16), edge, sidebarRect_.bottom - scaled(12)}, message, muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    } else if (activeSidebar_ == Sidebar::Branch && !branches_.child()) {
        const auto message = root_.empty() ? L"Open a folder to view branches." : !git_ || branchesLoading_ ? L"Loading branches…" :
            !git_->error.empty() ? L"Git unavailable.\n" + git_->error : git_->root.empty() ? L"This folder is not in a Git repository." :
            !branchError_.empty() ? branchError_ + L"\nPress F5 to retry." : L"No branches or commits yet.";
        dc.label({left, top + scaled(16), edge, sidebarRect_.bottom - scaled(12)}, message, muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
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
            dc.label({rect.left + scaled(12), rect.top, rect.right - scaled(28), rect.bottom}, title, active ? foreground : muted);
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
    explorerActivity_.select(sidebar == Sidebar::Explorer); gitActivity_.select(sidebar == Sidebar::Git); branchActivity_.select(sidebar == Sidebar::Branch);
    if (sidebar == Sidebar::Branch) refreshBranches();
    SetFocus(sidebar == Sidebar::Explorer ? (IsWindowVisible(tree_) ? tree_ : explorerButton_) : sidebar == Sidebar::Git ? (IsWindowVisible(gitTree_) ? gitTree_ : gitButton_) : (IsWindowVisible(branchTree_) ? branchTree_ : branchButton_));
}
RECT ProjectRuntime::Impl::gitViewButton(bool tree) const {
    const int padding = MulDiv(12, dpi_, 96), top = toolbar_ + sidebarHeader_;
    const int middle = (sidebarRect_.left + sidebarRect_.right) / 2;
    return {tree ? sidebarRect_.left + padding : middle, top + MulDiv(59, dpi_, 96),
        tree ? middle : sidebarRect_.right - padding, top + gitHeader_ - MulDiv(3, dpi_, 96)};
}
void ProjectRuntime::Impl::setGitView(bool tree) {
    if (gitTreeView_ == tree) return;
    gitTreeView_ = tree;
    updateGitFiles();
    if (IsWindowVisible(gitTree_)) SetFocus(gitTree_);
}
void ProjectRuntime::Impl::updateGitFiles() {
    std::wstring selected = wide(pendingGitSelection_);
    const int oldTop = changes_.top(), oldLeft = changes_.left();
    if (const auto node = changes_.item(changes_.selection()); node && node->data < gitChangeNodes_.size()) selected = gitChangeNodes_[node->data].path;
    updatingGitFiles_ = true;
    {
        Tree::Batch batch(changes_); changes_.clear(); gitChangeNodes_.clear();
        TreeItem selection = 0;
        if (git_ && git_->error.empty()) for (const bool staged : {true, false}) {
            const std::wstring prefix = staged ? L"staged:" : L"changes:";
            std::vector<std::wstring> paths; std::vector<size_t> entries;
            for (size_t i = 0; i < git_->entries.size(); ++i) {
                const auto& entry = git_->entries[i];
                if (staged ? !entry.staged : !entry.changed) continue;
                paths.push_back(entry.path.lexically_relative(git_->root).generic_wstring()); entries.push_back(i);
            }
            const size_t groupIndex = gitChangeNodes_.size();
            gitChangeNodes_.push_back({prefix, noGitChange, true, staged, true});
            const auto group = changes_.insert(0, (staged ? L"Staged Changes" : L"Changes") + (L" (" + std::to_wstring(entries.size()) + L")"), true, groupIndex);
            const auto nodes = buildGitChanges(paths, gitTreeView_);
            std::vector<TreeItem> items; items.reserve(nodes.size());
            for (const auto& node : nodes) {
                const auto parent = node.parent == noGitChange ? group : items[node.parent];
                const auto offset = node.parent == noGitChange ? 0 : nodes[node.parent].path.size() + 1;
                const auto index = gitChangeNodes_.size();
                const auto entryIndex = node.directory() ? noGitChange : entries[node.entry];
                gitChangeNodes_.push_back({prefix + node.path, entryIndex, node.directory(), staged, false});
                const auto state = node.directory() ? wchar_t{} : staged ? static_cast<wchar_t>(git_->entries[entryIndex].index) : git_->entries[entryIndex].state;
                const auto item = changes_.insert(parent, node.path.substr(offset), node.directory(), index, node.directory() ? foreground : stateColor(state));
                items.push_back(item);
                if (node.directory()) changes_.expand(item, !collapsedGitPaths_.count(prefix + node.path));
                if (prefix + node.path == selected || (!staged && node.path == selected)) selection = item;
            }
            changes_.expand(group, !collapsedGitPaths_.count(prefix));
            if (selected == prefix) selection = group;
        }
        for (auto item = selection; item;) {
            const auto node = changes_.item(item); item = node ? node->parent : 0;
            if (const auto parent = changes_.item(item); parent && !parent->expanded) selection = item;
        }
        changes_.select(selection, false);
    }
    updatingGitFiles_ = false;
    if (git_) { changes_.restoreScroll(restoreGitTop_ ? restoreGitTop_ : oldTop, restoreGitLeft_ ? restoreGitLeft_ : oldLeft); restoreGitTop_ = restoreGitLeft_ = 0; pendingGitSelection_.clear(); }
    layout(false);
}
void ProjectRuntime::Impl::openGitFile(TreeItem item) {
    if (updatingGitFiles_ || !git_) return;
    const auto value = changes_.item(item);
    if (!value || value->data >= gitChangeNodes_.size()) return;
    const auto& node = gitChangeNodes_[value->data];
    if (node.directory || node.entry >= git_->entries.size()) return;
    const auto entry = git_->entries[node.entry];
    if (entry.state == L'?') { openFile(entry.path); return; }
    openGitDiff(entry.path, node.staged ? GitDiffKind::Staged : GitDiffKind::Changes);
}
HistoryNode* ProjectRuntime::Impl::historyNode(TreeItem item) const {
    const auto value = branches_.item(item); return value ? reinterpret_cast<HistoryNode*>(value->data) : nullptr;
}
TreeItem ProjectRuntime::Impl::insertHistory(TreeItem parent, std::wstring text, HistoryNode node, bool expandable, COLORREF color) {
    auto data = std::make_unique<HistoryNode>(std::move(node));
    const auto item = branches_.insert(parent, std::move(text), expandable, reinterpret_cast<uintptr_t>(data.get()), color);
    if (item) data.release();
    return item;
}
void ProjectRuntime::Impl::refreshBranches() {
    if (!git_ || !git_->error.empty() || git_->root.empty() || (root_.wsl() && !connected_)) return;
    const auto root = git_->root; const auto generation = generation_, request = ++branchRequest_;
    branchesLoading_ = true;
    gitWorker_->submit(L"branches", [this, root, generation, request] {
        auto result = provider_->git({L"git.exe", L"--no-optional-locks", L"for-each-ref", L"--sort=refname",
            L"--format=%(refname)%00%(refname:short)%00%(objectname)%00%(HEAD)", L"refs/heads", L"refs/remotes"}, root, gitWorker_->cancellation());
        auto refs = parseGitBranches(result.output);
        // Detached HEAD remains browsable even when no branch points to it.
        if (!result.code && std::none_of(refs.begin(), refs.end(), [](const GitBranch& ref) { return ref.current; })) {
            auto head = provider_->git({L"git.exe", L"rev-parse", L"--verify", L"HEAD"}, root, gitWorker_->cancellation());
            if (!head.code) refs.insert(refs.begin(), {"HEAD", "HEAD (detached)", utf8(trimmed(head.output)), true});
        }
        const auto error = result.error; const auto code = result.code;
        gitWorker_->complete([this, generation, request, refs = std::move(refs), error, code] {
            if (generation != generation_ || request != branchRequest_) return;
            branchesLoading_ = false; branchError_ = code ? error : L"";
            if (code) { status(L"Branches: " + error); layout(false); return; }
            Tree::Batch batch(branches_);
            std::map<std::string, TreeItem> existing;
            for (auto item = branches_.child(); item; item = branches_.next(item)) if (const auto n = historyNode(item)) existing[n->ref] = item;
            for (const auto& ref : refs) {
                const auto found = existing.find(ref.ref);
                const auto text = (ref.current ? L"● " : L"") + wide(ref.name);
                if (found == existing.end()) {
                    const auto item = insertHistory(0, text, {HistoryKind::Branch, ref.ref, ref.tip, {}}, true, ref.current ? accent : foreground);
                    if (!branchesLoaded_ && ref.current) branches_.expand(item, true);
                } else {
                    const auto item = found->second; auto n = historyNode(item);
                    branches_.label(item, text, ref.current ? accent : foreground);
                    if (n->revision != ref.tip || (n->loading && n->generation != generation_)) {
                        n->revision = ref.tip; n->loaded = n->loading = false; n->count = 0;
                        branches_.clearChildren(item);
                        if (branches_.item(item)->expanded) requestHistory(item);
                    }
                    existing.erase(found);
                }
            }
            for (const auto& stale : existing) branches_.erase(stale.second);
            branches_.sort(0, [](uintptr_t a, uintptr_t b) { return reinterpret_cast<HistoryNode*>(a)->ref < reinterpret_cast<HistoryNode*>(b)->ref; });
            branchesLoaded_ = true; layout(false);
        });
    });
}
void ProjectRuntime::Impl::requestHistory(TreeItem item, bool more) {
    auto n = historyNode(item);
    if (n && n->loading && n->generation != generation_) n->loading = false;
    if (!n || n->loading || (n->loaded && !more) || !git_ || git_->root.empty()) return;
    if (n->kind != HistoryKind::Branch && n->kind != HistoryKind::Commit) return;
    n->loading = true; n->generation = generation_;
    const auto kind = n->kind; const auto revision = n->revision; const auto count = n->count;
    const auto root = git_->root; const auto generation = generation_;
    {
        Tree::Batch batch(branches_);
        if (!more) branches_.clearChildren(item);
        else for (auto child = branches_.child(item); child;) {
            const auto next = branches_.next(child); const auto value = historyNode(child);
            if (value && (value->kind == HistoryKind::More || value->kind == HistoryKind::Message)) branches_.erase(child);
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
            auto n = historyNode(item); if (!n || n->revision != revision) return;
            n->loading = false; n->loaded = code == 0;
            Tree::Batch batch(branches_);
            for (auto child = branches_.child(item); child;) {
                const auto next = branches_.next(child); const auto value = historyNode(child);
                if (value && value->kind == HistoryKind::Message) branches_.erase(child);
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
                std::vector<std::wstring> paths; std::vector<GitCommitFile> supported;
                for (const auto& file : files) {
                    if (root.wsl() && !validUtf8(file.path)) { status(L"Some commit filenames are not UTF-8 and cannot be opened."); continue; }
                    if (paths.size() >= maxTreeNodes) { status(L"Commit file tree limited to 50,000 files."); break; }
                    paths.push_back(wide(file.path)); supported.push_back(file);
                }
                const auto nodes = buildGitChanges(paths, true); std::vector<TreeItem> items;
                for (const auto& value : nodes) {
                    const auto parent = value.parent == noGitChange ? item : items[value.parent];
                    const auto offset = value.parent == noGitChange ? 0 : nodes[value.parent].path.size() + 1;
                    const auto child = insertHistory(parent, value.path.substr(offset),
                        {value.directory() ? HistoryKind::Directory : HistoryKind::File, {}, revision, root / std::string_view(utf8(value.path))}, value.directory(),
                        value.directory() ? foreground : stateColor(static_cast<wchar_t>(supported[value.entry].state)));
                    items.push_back(child); if (value.directory()) branches_.expand(child, true);
                }
                if (paths.empty()) insertHistory(item, L"No changed files.", {HistoryKind::Message, {}, {}, {}}, false, muted);
            }
        });
    });
}
void ProjectRuntime::Impl::openHistory(TreeItem item) {
    const auto n = historyNode(item); if (!n) return;
    if (n->kind == HistoryKind::File) openGitDiff(n->path, GitDiffKind::Commit, n->revision);
    else if (n->kind == HistoryKind::More) {
        const auto value = branches_.item(item); if (value) requestHistory(value->parent, true);
    } else if (n->kind == HistoryKind::Branch || n->kind == HistoryKind::Commit) {
        if (!branches_.item(item)->expanded) branches_.expand(item, true);
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
        MessageBoxW(window_, (path.native() + L"\n\n" + error).c_str(), L"Poppy — デフォルトのアプリケーションで開く", MB_OK | MB_ICONWARNING);
    }
}
void ProjectRuntime::Impl::status(std::wstring message) { status_ = std::move(message); RECT r{}; GetClientRect(window_, &r); r.top = r.bottom - footer_; InvalidateRect(window_, &r, FALSE); }
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
void ProjectRuntime::Impl::clearChildren(TreeItem parent) { explorer_.clearChildren(parent); }
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
    explorer_.label(item, std::move(text), state ? stateColor(state) : foreground);
}
void ProjectRuntime::Impl::renameFile(TreeItem item, const std::wstring& name) {
    const auto n = node(item); if (!n || n->unsupported || item == rootItem_ || renamePending_) return;
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { status(L"Reconnect WSL before renaming."); return; }
    std::wstring error = L"Enter a valid Linux filename (no /, NUL, . or ..; at most 255 UTF-8 bytes).";
    if (!(root_.wsl() ? validPosixName(utf8(name)) : validFileName(name, error))) { MessageBoxW(window_, error.c_str(), L"Poppy — Rename", MB_OK | MB_ICONWARNING); return; }
    const auto source = n->path, destination = source.parent_path() / name;
    if (source.native() == destination.native()) return;
    saveTab(); endTabDrag(); renamePending_ = true; status(L"Renaming " + source.filename().native() + L"…");
    const auto generation = generation_;
    struct RenamedBuffer { ResourceId path; std::shared_ptr<EditBuffer> buffer; std::string baseline, version; };
    std::vector<RenamedBuffer> buffers;
    if (root_.wsl()) for (const auto& tab : tabs_) if (tab.buffer && source.contains(tab.path)) buffers.push_back({relocateResource(tab.path, source, destination), tab.buffer, tab.buffer->baseline, {}});
    worker_->submit(L"rename", [this, source, destination, generation, buffers = std::move(buffers)]() mutable {
        // Deliberately omit MOVEFILE_REPLACE_EXISTING: collisions must never overwrite.
        const auto error = provider_->renameResource(source, destination);
        if (error.empty()) for (auto& item : buffers) { const auto file = provider_->readFile(item.path); if (file.error.empty() && file.bytes == item.baseline) item.version = file.version; }
        worker_->complete([this, source, destination, generation, error, buffers = std::move(buffers)] {
            renamePending_ = false; if (generation != generation_) return;
            if (!error.empty()) { status(L"Rename failed"); MessageBoxW(window_, (source.native() + L"\n\n" + error).c_str(), L"Poppy — Rename", MB_OK | MB_ICONWARNING); return; }
            for (const auto& item : buffers) if (!item.version.empty() && item.buffer->baseline == item.baseline) item.buffer->fileVersion = item.version;
            saveTab(); ++generation_; ++serial_[0]; ++serial_[1];
            std::map<std::wstring, ResourceId> expanded;
            for (const auto& pair : expandedPaths_) { auto path = relocateResource(pair.second, source, destination); expanded[pathKey(path)] = std::move(path); }
            expandedPaths_ = std::move(expanded); pendingTreeSelection_ = destination;
            {
                Tree::Batch batch(explorer_);
                for (const auto& pair : nodes_) if (auto current = node(pair.second)) {
                    current->path = relocateResource(current->path, source, destination); current->loading = false; updateLabel(pair.second);
                }
                for (const auto& pair : nodes_) if (const auto current = node(pair.second); current && current->directory) explorer_.sort(pair.second, [](uintptr_t a, uintptr_t b) {
                    const auto x = reinterpret_cast<Node*>(a), y = reinterpret_cast<Node*>(b);
                    if (x->directory != y->directory) return x->directory;
                    return CompareStringOrdinal(x->path.filename().native().c_str(), -1, y->path.filename().native().c_str(), -1, x->path.wsl() ? FALSE : TRUE) == CSTR_LESS_THAN;
                });
            }
            for (auto& tab : tabs_) {
                if (tab.diffKind == GitDiffKind::Commit) continue;
                const auto path = relocateResource(tab.path, source, destination);
                if (path == tab.path) continue;
                tab.path = path;
                if (tab.buffer) { tab.buffer->document.language = path.filename() == L"CMakeLists.txt" ? Language::Config : languageFor(utf8(path.extension().native())); indexDocument(tab.buffer->document); }
            }
            terminal_.relocate(source, destination);
            layout(); refresh();
            status(L"Renamed to " + destination.filename().native());
            if (owner_ && owner_->changed) owner_->changed();
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
    auto saved = std::move(initialState_);
    restoreProject(saved.value_or(ProjectSession{}));
    if (!saved) expandedPaths_[pathKey(root_)] = root_;
    viewer().message(L"Select a file on the left.\n\nEdit here and save with Ctrl+S, or use your AI CLI in the terminal.");
    updateGitFiles();
    rootItem_ = insert(0, {root_, true, false, false, {}});
    if (expandedPaths_.count(pathKey(root_))) requestDirectory(rootItem_);
    if (pathKey(pendingTreeSelection_) == pathKey(root_)) { explorer_.select(rootItem_, false); pendingTreeSelection_.clear(); }
    terminal_.setDirectory(root_); watch(); if (connected_) quickOpen_.setRoot(root_); refreshGit();
    SetWindowTextW(window_, (L"Poppy — " + root_.native()).c_str()); status(L"F5: refresh  ·  F6: diff  ·  Ctrl+`: terminal"); InvalidateRect(window_, nullptr, FALSE);
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
                const auto previous = root_; root_ = resolved; connected_ = true; wslUid_ = provider_->identity(); providerRevision_ = provider_->revision();
                std::map<std::wstring, ResourceId> expanded;
                for (const auto& item : expandedPaths_) { auto path = relocateResource(item.second, previous, root_); expanded[pathKey(path)] = std::move(path); }
                expandedPaths_ = std::move(expanded); expandedPaths_[pathKey(root_)] = root_;
                pendingTreeSelection_ = relocateResource(pendingTreeSelection_, previous, root_);
                for (auto& tab : tabs_) tab.path = relocateResource(tab.path, previous, root_);
                for (const auto& pair : nodes_) if (auto n = node(pair.second)) { n->path = relocateResource(n->path, previous, root_); n->loading = false; updateLabel(pair.second); }
                terminal_.relocate(previous, root_); terminal_.setDirectory(root_); quickOpen_.setRoot(root_);
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
    state.gitSidebar = activeSidebar_ == Sidebar::Git; state.branchSidebar = activeSidebar_ == Sidebar::Branch; state.activeTab = activeTab_;
    state.diffInline = viewer().diffInline(); state.gitTree = gitTreeView_; state.firstTabRow = editorGroups_[0].firstRow; state.rightTabRow = editorGroups_[1].firstRow; state.editorRatio = editorRatio_;
    for (const auto& tab : tabs_) state.tabs.push_back({tab.path.u8string(), tab.diff, tab.fileView, tab.diffView, tab.pinned, tab.group, tab.displayed, tab.diffKind, tab.revision, tab.allChanges});
    for (const auto& entry : expandedPaths_) state.expanded.push_back(entry.second.u8string());
    if (!pendingTreeSelection_.empty()) state.selected = pendingTreeSelection_.u8string();
    else if (const auto selected = node(explorer_.selection())) state.selected = selected->path.u8string();
    state.explorerTop = restoredScroll_ ? explorer_.top() : restoreExplorerTop_; state.explorerLeft = restoredScroll_ ? explorer_.left() : restoreExplorerLeft_;
    state.gitTop = git_ ? changes_.top() : restoreGitTop_; state.gitLeft = git_ ? changes_.left() : restoreGitLeft_;
    if (const auto selected = changes_.item(changes_.selection()); selected && selected->data < gitChangeNodes_.size()) state.gitSelected = utf8(gitChangeNodes_[selected->data].path);
    else state.gitSelected = pendingGitSelection_;
    for (const auto& path : collapsedGitPaths_) state.collapsedGit.push_back(utf8(path));
    const auto focus = GetFocus();
    if (focus == terminal_.window()) focusedPane_ = "terminal";
    else if (focus == tree_) focusedPane_ = "explorer";
    else if (focus == gitTree_) focusedPane_ = "git";
    else if (focus == branchTree_) focusedPane_ = "branch";
    else if (focus == viewer().window() || IsChild(viewer().window(), focus)) focusedPane_ = "viewer";
    state.focusedPane = focusedPane_;
    terminal_.snapshot(state);
    return state;
}
void ProjectRuntime::Impl::restoreProject(const ProjectSession& state) {
    wslUid_ = state.wslUid;
    focusedPane_ = state.focusedPane;
    editorGroups_[0].firstRow = state.firstTabRow; editorGroups_[1].firstRow = state.rightTabRow; editorRatio_ = state.editorRatio;
    restoreExplorerTop_ = state.explorerTop; restoreExplorerLeft_ = state.explorerLeft;
    restoreGitTop_ = state.gitTop; restoreGitLeft_ = state.gitLeft; pendingGitSelection_ = state.gitSelected;
    for (const auto& path : state.collapsedGit) collapsedGitPaths_.insert(wide(path));
    terminal_.setDirectory(root_); terminal_.restore(state);
    for (auto& view : viewers_) view.setDiffLayout(state.diffInline);
    gitTreeView_ = state.gitTree;
    sidebar_ = state.sidebar; terminalHeight_ = state.terminalHeight; terminalVisible_ = state.terminalVisible;
    activeSidebar_ = state.branchSidebar ? Sidebar::Branch : state.gitSidebar ? Sidebar::Git : Sidebar::Explorer;
    explorerActivity_.select(activeSidebar_ == Sidebar::Explorer); gitActivity_.select(activeSidebar_ == Sidebar::Git); branchActivity_.select(activeSidebar_ == Sidebar::Branch);
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
        Tab target{path, saved.diff, saved.fileView, saved.diffView, saved.pinned, {}, saved.group, saved.displayed, saved.diffKind, saved.revision, saved.allChanges};
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
        auto result = std::make_shared<DirectoryResult>(provider_->listDirectory(path));
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
        });
    });
}
void ProjectRuntime::Impl::applyDirectory(TreeItem item, Node*, const DirectoryResult& result) {
    if (!result.error.empty()) { status(L"Folder: " + result.error); return; }
    std::map<std::wstring, TreeItem> existing;
    for (auto child = explorer_.child(item); child; child = explorer_.next(child)) if (const auto n = node(child)) existing[n->unsupported ? L"unsupported:" + n->label : pathKey(n->path)] = child;
    Tree::Batch batch(explorer_);
    for (const auto& entry : result.entries) {
        const auto found = existing.find(entry.unsupported ? L"unsupported:" + entry.label : pathKey(entry.path));
        if (found != existing.end()) {
            auto n = node(found->second);
            if (n && n->directory == entry.directory && n->reparse == entry.reparse && n->unsupported == entry.unsupported) { existing.erase(found); continue; }
            if (n) forgetExpanded(n->path);
            explorer_.erase(found->second); existing.erase(found);
        }
        insert(item, entry);
    }
    for (const auto& stale : existing) { if (const auto n = node(stale.second)) forgetExpanded(n->path); explorer_.erase(stale.second); }
    explorer_.sort(item, [](uintptr_t a, uintptr_t b) {
        const auto x = reinterpret_cast<Node*>(a), y = reinterpret_cast<Node*>(b);
        if (x->directory != y->directory) return x->directory;
        return CompareStringOrdinal(x->path.filename().native().c_str(), -1, y->path.filename().native().c_str(), -1, x->path.wsl() ? FALSE : TRUE) == CSTR_LESS_THAN;
    });
    for (auto child = explorer_.child(item); child; child = explorer_.next(child)) {
        const auto n = node(child); if (!n) continue;
        if (n->directory && !n->loaded && !n->loading && expandedPaths_.count(pathKey(n->path))) requestDirectory(child);
        if (!pendingTreeSelection_.empty() && pathKey(n->path) == pathKey(pendingTreeSelection_)) {
            explorer_.select(child, false); pendingTreeSelection_.clear();
        }
    }
    if (result.truncated || nodes_.size() >= maxTreeNodes) status(L"Explorer limit: 10,000 items/folder, 50,000 visible nodes.");
}
void ProjectRuntime::Impl::normalizeGroups() {
    if (tabs_.empty()) { focusedGroup_ = 0; activeTab_ = 0; return; }
    if (std::none_of(tabs_.begin(), tabs_.end(), [](const Tab& t) { return t.group == 0; })) {
        for (auto& tab : tabs_) tab.group = 0;
        focusedGroup_ = 0; ++serial_[0]; ++serial_[1];
        viewers_[0].message(L"Loading…"); viewers_[1].message(L"");
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
        focusedGroup_ = targetGroup; ++serial_[0]; ++serial_[1];
        normalizeGroups(); layout(); loadTab(false, 0); loadTab(false, 1); SetFocus(viewer().window()); return;
    }
    layout();
}
void ProjectRuntime::Impl::updateTabs(bool reveal) {
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
        if (!tab.diff && view.buffer()) tab.buffer = view.buffer();
    }
}
bool ProjectRuntime::Impl::saveFile(size_t index, bool wait) {
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
            if (error.empty()) { buffer->saved(bytes, text); buffer->fileVersion = version; status(L"Saved " + path.filename().native()); refreshGit(); }
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
    const size_t index = static_cast<size_t>(found - tabs_.begin());
    if (found != tabs_.end() && index == activeTab_ && (!diff || found->diff)) { updateTabs(); return; }
    saveTab();
    if (found == tabs_.end()) { Tab tab; tab.path = path; tab.diff = diff; tab.group = focusedGroup_; tabs_.push_back(std::move(tab)); }
    else if (diff) found->diff = true;
    focusedGroup_ = tabs_[index].group; for (auto& tab : tabs_) if (tab.group == focusedGroup_) tab.displayed = false; tabs_[index].displayed = true; activeTab_ = index; layout(); loadTab();
}
void ProjectRuntime::Impl::openGitDiff(const ResourceId& path, GitDiffKind kind, std::string revision, bool allChanges) {
    if (path.empty()) return;
    endTabDrag(); saveTab();
    Tab target; target.path = path; target.diff = true; target.diffKind = kind; target.revision = std::move(revision); target.allChanges = allChanges; target.group = focusedGroup_;
    const auto key = tabKey(target);
    const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tabKey(tab) == key; });
    const size_t index = static_cast<size_t>(found - tabs_.begin());
    if (found == tabs_.end()) tabs_.push_back(std::move(target));
    focusedGroup_ = tabs_[index].group;
    for (auto& tab : tabs_) if (tab.group == focusedGroup_) tab.displayed = false;
    tabs_[index].displayed = true; activeTab_ = index; layout(); loadTab();
}
void ProjectRuntime::Impl::selectTab(size_t index) {
    endTabDrag();
    if (index >= tabs_.size() || index == activeTab_) return;
    saveTab(); focusedGroup_ = tabs_[index].group; for (auto& tab : tabs_) if (tab.group == focusedGroup_) tab.displayed = false; tabs_[index].displayed = true; activeTab_ = index; updateTabs(); loadTab();
}
void ProjectRuntime::Impl::closeTab(size_t index) {
    endTabDrag();
    if (index >= tabs_.size() || tabs_[index].pinned || !confirmTab(index)) return;
    saveTab(); const bool active = tabs_[index].displayed; const int group = tabs_[index].group;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < activeTab_) --activeTab_;
    else if (activeTab_ >= tabs_.size()) activeTab_ = tabs_.empty() ? 0 : tabs_.size() - 1;
    if (active && !tabs_.empty()) {
        size_t replacement = tabs_.size();
        for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].group == group) { replacement = i; if (i >= index) break; }
        if (replacement < tabs_.size()) tabs_[replacement].displayed = true;
    }
    layout();
    if (active) {
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
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, pinned ? L"Unpin tab" : L"Pin tab");
    if (!tabs_[index].allChanges && tabs_[index].diffKind != GitDiffKind::Commit) AppendMenuW(menu, MF_STRING, 3, L"デフォルトのアプリケーションで開く");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (pinned ? MF_GRAYED : 0), 2, L"Close tab");
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(menu);
    const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tabKey(tab) == key; });
    if (found != tabs_.end()) {
        index = static_cast<size_t>(found - tabs_.begin());
        if (command == 1) toggleTabPin(index);
        else if (command == 2) closeTab(index);
        else if (command == 3) openWithDefaultApp(path);
    }
}
void ProjectRuntime::Impl::loadTab(bool preserve, int group) {
    if (group < 0) group = focusedGroup_;
    waitingForGit_[group] = false;
    const size_t index = groupTab(group); if (index >= tabs_.size()) { viewers_[group].message(L"Select a file."); return; }
    preserve = preserve && viewers_[group].hasDocument();
    const auto& tab = tabs_[index]; const auto path = tab.path;
    const auto state = tab.diff ? tab.diffView : tab.fileView;
    const auto serial = ++serial_[group], generation = generation_;
    if (!tab.diff && tab.buffer && (tab.buffer->dirty() || tab.buffer->saving)) {
        viewers_[group].edit(tab.buffer, path, preserve); if (!preserve) viewers_[group].restore(state);
        status(L"Unsaved changes kept  ·  Ctrl+S: save"); return;
    }
    if (root_.wsl() && !connected_) { if (!viewers_[group].hasDocument()) viewers_[group].message(connecting_ ? L"Connecting to WSL…" : L"WSL disconnected. Press F5 to reconnect.", path); return; }
    // Retain only tab metadata and view positions; reload the active document.
    if (!preserve) viewers_[group].message(tab.diff ? L"Loading Git diff…" : L"Loading…", path);
    status((tab.diff ? L"Git diff: " : L"Loading ") + path.filename().native());
    if (!tab.diff) {
        worker_->submit(L"file:" + std::to_wstring(group), [this, group, path, serial, generation, preserve, state] {
            auto result = readDocument(*provider_, path);
            worker_->complete([this, group, result, path, serial, generation, preserve, state] {
                if (generation != generation_ || serial != serial_[group]) return;
                if (viewers_[group].buffer() && (viewers_[group].buffer()->dirty() || viewers_[group].buffer()->saving)) return;
                if (result->document) {
                    tabs_[groupTab(group)].buffer = result->buffer;
                    viewers_[group].edit(result->buffer, path, preserve); if (!preserve) viewers_[group].restore(state);
                    status(std::to_wstring(result->document->lines.size()) + L" lines  ·  Ctrl+S: save  ·  F6: diff");
                } else { viewers_[group].message(result->error, path); status(L"Preview unavailable"); }
            });
        });
        return;
    }
    if (!git_) { waitingForGit_[group] = true; viewers_[group].message(L"Loading Git repository…", path); return; }
    if (git_->root.empty() || !git_->error.empty()) { viewers_[group].message(L"Git repository unavailable. Check Git in the project environment and press F5.", path); return; }
    const auto root = git_->root;
    const auto kind = tab.diffKind; const auto revision = tab.revision; const auto allChanges = tab.allChanges;
    const auto title = tabTitle(tab);
    gitWorker_->submit(L"diff:" + std::to_wstring(group), [this, group, serial, generation, path, root, preserve, state, kind, revision, allChanges, title] {
        constexpr size_t maxPatchBytes = 4 * 1024 * 1024;
        const auto relative = allChanges ? std::string{} : path.lexically_relative(root).generic_u8string();
        auto readDiff = [&](GitDiffKind scope) { return provider_->git(gitArguments(gitDiffArguments(scope, revision, relative)), root, gitWorker_->cancellation()); };
        auto scope = kind == GitDiffKind::Automatic ? GitDiffKind::Changes : kind;
        auto result = readDiff(scope);
        if (kind == GitDiffKind::Automatic && result.code == 0 && result.output.empty()) { scope = GitDiffKind::Staged; result = readDiff(scope); }
        std::vector<ResourceId> untracked;
        if (!result.code && allChanges && kind == GitDiffKind::Changes) {
            auto status = provider_->git({L"git.exe", L"--no-optional-locks", L"-c", L"core.fsmonitor=false", L"status", L"--porcelain=v1", L"-z", L"--untracked-files=all"}, root, gitWorker_->cancellation());
            if (status.code) { result.code = status.code; result.error = status.error; }
            else for (const auto& entry : parseGitStatus(status.output)) if (entry.index == '?' && (!root.wsl() || validUtf8(entry.path))) {
                untracked.push_back(root / std::string_view(entry.path));
                if (untracked.size() >= maxTreeNodes) break;
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
        if (result.code == 0) { doc->patch.text = std::move(result.output); indexDiff(*doc, allChanges || kind != GitDiffKind::Automatic); }
        const auto error = result.error; const auto code = result.code;
        gitWorker_->complete([this, group, serial, generation, path, doc, code, error, scope, preserve, state, kind, allChanges, title] {
            if (generation != generation_ || serial != serial_[group]) return;
            if (code) viewers_[group].message(error, path);
            else if (doc->patch.text.empty()) viewers_[group].message(L"No changes to display.", path);
            else if (doc->rows.empty()) viewers_[group].message(L"No text changes to display.", path);
            else { viewers_[group].showDiff(doc, path, preserve, kind == GitDiffKind::Automatic ? L"" : title); if (!preserve) viewers_[group].restore(state); }
            status((scope == GitDiffKind::Commit ? L"Commit diff" : scope == GitDiffKind::Staged ? L"Staged diff" : L"Working tree diff") +
                std::wstring(allChanges ? L" · All changes" : L"") + L" · F7: split / inline");
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
    if (!tab.diff && (!git_ || git_->root.empty() || !git_->error.empty())) { status(L"Git repository unavailable. Check Git in the project environment and press F5."); return; }
    saveTab(); tab.diff = !tab.diff; layout(); loadTab();
}
void ProjectRuntime::Impl::refreshGit() {
    if (root_.empty() || (root_.wsl() && !connected_)) return;
    const auto generation = generation_; const auto root = root_;
    gitWorker_->submit(L"git", [this, root, generation] {
        auto result = readGit(*provider_, root, gitWorker_->cancellation());
        gitWorker_->complete([this, generation, result] {
            if (generation != generation_) return;
            git_ = result;
            if (result->root.empty() || !result->error.empty()) {
                branches_.clear(); branchesLoaded_ = branchesLoading_ = false; branchError_.clear(); ++branchRequest_;
            }
            { Tree::Batch batch(explorer_); for (const auto& pair : nodes_) updateLabel(pair.second); }
            updateGitFiles();
            if (activeSidebar_ == Sidebar::Branch || branchesLoaded_) refreshBranches();
            for (int group = 0; group < 2; ++group) if (waitingForGit_[group]) loadTab(false, group);
            if (!result->error.empty()) status(L"Git: " + result->error);
            else if (result->unsupportedNames) status(L"Some Git filenames are not UTF-8 and cannot be opened.");
            else if (result->truncated) status(L"Git display limited to 50,000 files and badge paths.");
            InvalidateRect(window_, nullptr, FALSE);
        });
    });
}
void ProjectRuntime::Impl::refresh(bool force) {
    if (root_.empty()) return;
    if (root_.wsl() && (!connected_ || provider_->state() != ConnectionState::Ready)) { if (force) connectRemote(); return; }
    if (force || filesDirty_) quickOpen_.refresh();
    filesDirty_ = false;
    for (const auto& pair : nodes_) { const auto n = node(pair.second); if (n && n->expandRequested) requestDirectory(pair.second); }
    refreshGit();
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
        const auto revision = provider_->revision();
        if (revision != providerRevision_) {
            providerRevision_ = revision;
            if (connected_ && provider_->state() != ConnectionState::Ready) { connected_ = false; ++generation_; ++serial_[0]; ++serial_[1]; status(L"WSL disconnected. Your edits have been kept. Use F5 to reconnect."); if (owner_ && owner_->changed) owner_->changed(); }
            else if (connected_) { if (!dirty_) dirtySince_ = now; dirty_ = fileDirty_ = filesDirty_ = true; lastEvent_ = now; }
        }
    }
    if (watchPending_ && WaitForSingleObject(watchEvent_.value, 0) == WAIT_OBJECT_0) {
        DWORD bytes = 0; const bool ok = GetOverlappedResult(watchDirectory_.value, &watchOperation_, &bytes, FALSE) != FALSE;
        bool relevant = !ok || !bytes;
        if (relevant) fileDirty_ = filesDirty_ = true;
        for (size_t offset = 0; ok && offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytes;) {
            const auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(watchBuffer_.data() + offset);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const bool gitNoise = name.rfind(L".git\\", 0) == 0 && name != L".git\\HEAD" && name != L".git\\index" && name.rfind(L".git\\refs\\", 0) != 0;
            if (!gitNoise) relevant = true;
            if (!gitNoise && info->Action != FILE_ACTION_MODIFIED) filesDirty_ = true;
            if (!tabs_.empty()) {
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
    case WM_APP + 12: if (!root_.empty() && IsWindowVisible(window_)) { if (root_.wsl()) { if (connected_) { fileDirty_ = filesDirty_ = true; refresh(false); } } else { refreshGit(); if (!tabs_.empty()) { saveTab(); loadTab(true, 0); loadTab(true, 1); } } } return 0;
    case WM_LBUTTONDOWN: {
        quickOpen_.close();
        const int x = MulDiv(GET_X_LPARAM(b), 96, dpi_), y = GET_Y_LPARAM(b);
        if (y < toolbar_) {
            if (x >= 98 && x < 210) chooseFolder(); else if (x >= 220 && x < 292) refresh(); else if (x >= 305 && x < 360) showDiff(); else if (x >= 374 && x < 460) toggleTerminal();
            else if (x >= 480 && x < 566) quickOpen_.show();
            return 0;
        }
        const POINT point{GET_X_LPARAM(b), y};
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
            else terminalHeight_ = std::clamp(MulDiv(r.bottom - footer_ - GET_Y_LPARAM(b), 96, dpi_), 100, std::max(100, MulDiv(r.bottom - toolbar_ - footer_, 96, dpi_) - 120));
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
        KillTimer(window_, timerId); stopWatch(); quickOpen_.stop(); if (gitWorker_) gitWorker_->stop(); if (worker_) worker_->stop(); terminal_.stop(); explorer_.clear(); changes_.clear(); branches_.clear();
        return 0;
    default: break;
    }
    return DefWindowProcW(window_, m, a, b);
}
bool ProjectRuntime::Impl::key(const MSG& message) {
        if (terminal_.key(message)) return true;
        const bool key = message.message == WM_KEYDOWN;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (key && message.wParam == VK_ESCAPE && tabDrag_) { endTabDrag(); return true; }
        if (key && ctrl && !shift && !(GetKeyState(VK_MENU) & 0x8000) && message.wParam == 'P') { quickOpen_.show(); return true; }
        if (key && ctrl && message.wParam == 'O') { quickOpen_.close(); chooseFolder(); return true; }
        if (quickOpen_.visible() && key) return false;
        if (GetFocus() != terminal_.window() && viewer().findKey(message)) return true;
        if (key && !ctrl && IsChild(viewer().window(), message.hwnd) && message.wParam == VK_TAB) return false;
        if (key && ctrl && shift && message.wParam == 'E') { selectSidebar(Sidebar::Explorer); return true; }
        if (key && ctrl && shift && message.wParam == 'G') { selectSidebar(Sidebar::Git); return true; }
        if (key && ctrl && shift && message.wParam == 'B') { selectSidebar(Sidebar::Branch); return true; }
        if (key && ctrl && message.wParam == VK_OEM_3) { toggleTerminal(); return true; }
        if (key && ctrl && message.wParam == VK_TAB && !tabs_.empty()) { selectTab((activeTab_ + (shift ? tabs_.size() - 1 : 1)) % tabs_.size()); SetFocus(viewer().window()); return true; }
        if (key && ctrl && message.wParam == 'W' && GetFocus() != terminal_.window()) { closeTab(activeTab_); return true; }
        if (key && message.wParam == VK_F5 && GetFocus() != terminal_.window()) { refresh(); return true; }
        if (key && message.wParam == VK_F6 && GetFocus() != terminal_.window()) { showDiff(); return true; }
        if (key && message.wParam == VK_F7 && GetFocus() != terminal_.window()) { const bool inlineView = !viewer().diffInline(); for (auto& view : viewers_) view.setDiffLayout(inlineView); return true; }
        if (key && message.wParam == VK_TAB && GetFocus() != terminal_.window() && GetFocus() != viewers_[0].window() && GetFocus() != viewers_[1].window()) {
            const HWND files = activeSidebar_ == Sidebar::Explorer ? (IsWindowVisible(tree_) ? tree_ : explorerButton_) : activeSidebar_ == Sidebar::Git ? (IsWindowVisible(gitTree_) ? gitTree_ : gitButton_) : (IsWindowVisible(branchTree_) ? branchTree_ : branchButton_);
            SetFocus(GetFocus() == files ? viewer().window() : terminalVisible_ ? terminal_.window() : files); return true;
        }

    return false;
}
ProjectRuntime::ProjectRuntime() : impl_(std::make_unique<Impl>()) { impl_->owner_ = this; }
ProjectRuntime::~ProjectRuntime() {
    shutdown();
    if (impl_->window_ && IsWindow(impl_->window_)) DestroyWindow(impl_->window_);
}
void ProjectRuntime::create(HWND parent, HINSTANCE instance, HookServer* hooks) {
    impl_->instance_ = instance;
    WNDCLASSW cls{}; cls.lpfnWndProc = Impl::procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyProject"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
    impl_->window_ = CreateWindowExW(0, cls.lpszClassName, L"Project", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr, instance, impl_.get());
    impl_->terminal_.connect(hooks);
}
void ProjectRuntime::shutdown() {
    impl_->stopWatch(); impl_->quickOpen_.stop();
    if (impl_->gitWorker_) impl_->gitWorker_->stop();
    if (impl_->worker_) impl_->worker_->stop();
    impl_->terminal_.stop();
}
bool ProjectRuntime::canClose() { if (impl_->renamePending_) { impl_->worker_->drain(); impl_->worker_->dispatch(); } impl_->saveTab(); for (size_t i = 0; i < impl_->tabs_.size(); ++i) if (!impl_->confirmTab(i)) return false; return true; }
void ProjectRuntime::revalidate() { PostMessageW(window(), WM_APP+13, 0, 0); }
void ProjectRuntime::initialize(const ResourceId& path, const ProjectSession& state, bool saved) { if (saved) impl_->initialState_ = state; impl_->openFolder(path); }
void ProjectRuntime::activate(bool active) {
    impl_->active_ = active;
    if (active && impl_->root_.wsl() && !impl_->connected_ && impl_->provider_ && impl_->provider_->state() == ConnectionState::Disconnected) impl_->connectRemote();
    if (!active) {
        impl_->saveTab(); impl_->lastFocus_ = GetFocus();
        if (impl_->lastFocus_ == impl_->terminal_.window()) impl_->focusedPane_ = "terminal";
        else if (impl_->lastFocus_ == impl_->tree_) impl_->focusedPane_ = "explorer";
        else if (impl_->lastFocus_ == impl_->gitTree_) impl_->focusedPane_ = "git";
        else if (impl_->lastFocus_ == impl_->branchTree_) impl_->focusedPane_ = "branch";
        else if (impl_->lastFocus_ == impl_->viewer().window() || IsChild(impl_->viewer().window(), impl_->lastFocus_)) impl_->focusedPane_ = "viewer";
        impl_->quickOpen_.close(false); impl_->endTabDrag(); ShowWindow(window(), SW_HIDE);
    }
    else {
        ShowWindow(window(), SW_SHOWNA); impl_->layout(false);
        HWND focus = impl_->lastFocus_;
        if (!focus || !IsWindow(focus) || !IsWindowVisible(focus)) {
            focus = impl_->focusedPane_ == "terminal" && impl_->terminalVisible_ ? impl_->terminal_.window() : impl_->focusedPane_ == "explorer" && IsWindowVisible(impl_->tree_) ? impl_->tree_ : impl_->focusedPane_ == "git" && IsWindowVisible(impl_->gitTree_) ? impl_->gitTree_ : impl_->focusedPane_ == "branch" && IsWindowVisible(impl_->branchTree_) ? impl_->branchTree_ : impl_->viewer().window();
        }
        SetFocus(focus); impl_->terminal_.acknowledge(); PostMessageW(window(), WM_APP+12, 0, 0);
    }
}
void ProjectRuntime::setDpi(UINT dpi) { impl_->dpi(dpi); }
void ProjectRuntime::poll() { if (ready()) impl_->pollWatch(); if (IsWindowVisible(impl_->terminal_.container())) { impl_->terminal_.acknowledge(); if (summary().counts[static_cast<size_t>(AiState::Running)]) InvalidateRect(impl_->terminal_.container(), nullptr, FALSE); } }
bool ProjectRuntime::key(const MSG& message) { return impl_->key(message); }
ProjectSession ProjectRuntime::snapshot() { return impl_->saveSession(); }
AiSummary ProjectRuntime::summary() const { return impl_->terminal_.summary(); }
std::wstring ProjectRuntime::description() const { return (impl_->root_.wsl() ? L"\n" + (impl_->connectionError_.empty() ? impl_->provider_->error() : impl_->connectionError_) : L"") + impl_->terminal_.description(); }
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
    for (auto& tab : impl_->tabs_) tab.buffer.reset();
    for (int group = 0; group < 2; ++group) { ++impl_->serial_[group]; impl_->waitingForGit_[group] = false; impl_->viewers_[group].message(L"Loading…"); }
}
HWND ProjectRuntime::window() const { return impl_->window_; }
bool ProjectRuntime::ready() const { return !impl_->root_.empty(); }
bool ProjectRuntime::hasUnsaved() const { return impl_->renamePending_ || std::any_of(impl_->tabs_.begin(), impl_->tabs_.end(), [](const Tab& tab) { return tab.buffer && (tab.buffer->dirty() || tab.buffer->saving); }); }
std::wstring ProjectRuntime::connectionDescription() const {
    if (!impl_->root_.wsl()) return {};
    return impl_->connecting_ ? L"Connecting" : impl_->connected_ && impl_->provider_->state() == ConnectionState::Ready ? L"Connected" : impl_->connectionError_.empty() && impl_->provider_->error().empty() ? L"Disconnected" : L"Unavailable";
}

}
