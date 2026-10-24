#include "viewer.h"
#include "terminal_pane.h"
#include "project_runtime.h"
#include "tree.h"
#include "git_changes.h"
#include "activity_button.h"
#include "quick_open.h"
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
constexpr int explorerCommand = 2, gitCommand = 3;
enum class Sidebar { Explorer, Git };
struct Entry { fs::path path; bool directory = false, reparse = false; };
struct Node {
    fs::path path; uint64_t id;
    bool directory, loaded = false, loading = false, reparse = false;
    // Keep expansion intent while loading, even when an empty item cannot expand.
    bool expandRequested = false;
};
struct DirectoryResult { std::vector<Entry> entries; std::wstring error; bool truncated = false; };
struct FileResult { std::shared_ptr<Document> document; std::wstring error; };
struct Tab { fs::path path; bool diff = false; Viewer::State fileView, diffView; bool pinned = false; };
struct TabDrop { size_t position; RECT marker; };
struct TabDrag {
    size_t index;
    POINT origin, point;
    bool active = false;
    std::optional<TabDrop> target;
};
struct GitEntry { fs::path path; wchar_t state; };
struct GitResult { fs::path root; std::wstring branch, error; std::map<std::wstring, wchar_t> states; std::vector<GitEntry> entries; size_t changed = 0, untracked = 0; bool truncated = false; };

COLORREF stateColor(wchar_t state) {
    return state == L'U' || state == L'D' ? RGB(231,112,118) : state == L'?' || state == L'A' ? accent : RGB(224,192,128);
}

std::shared_ptr<DirectoryResult> listDirectory(const fs::path& path) {
    auto result = std::make_shared<DirectoryResult>(); std::error_code error;
    fs::directory_iterator iterator(path, fs::directory_options::skip_permission_denied, error), end;
    if (error) { result->error = wide(error.message()); return result; }
    for (; iterator != end; iterator.increment(error)) {
        if (error) { result->error = wide(error.message()); break; }
        const auto& entry = *iterator;
        if (entry.path().filename() == L".git") continue;
        if (result->entries.size() == maxEntries) { result->truncated = true; break; }
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        result->entries.push_back({entry.path(), (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
    }
    std::sort(result->entries.begin(), result->entries.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        return CompareStringOrdinal(a.path.filename().c_str(), -1, b.path.filename().c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return result;
}
std::shared_ptr<FileResult> readDocument(const fs::path& path) {
    auto result = std::make_shared<FileResult>();
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) { result->error = winError(); return result; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size)) { result->error = winError(); return result; }
    if (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(maxFileBytes)) { result->error = L"This file exceeds the 16 MiB preview limit.\nOpen a smaller file to keep Poppy responsive."; return result; }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0'); size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        if (!ReadFile(file.value, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr)) { result->error = winError(); return result; }
        if (!count) break;
        offset += count;
    }
    bytes.resize(offset); auto document = std::make_shared<Document>(); std::string error;
    if (!decodeText(std::move(bytes), document->text, document->encoding, error)) { result->error = wide(error); return result; }
    if (static_cast<size_t>(std::count(document->text.begin(), document->text.end(), '\n')) >= maxLines) { result->error = L"This file exceeds the 1,000,000-line preview limit."; return result; }
    document->language = path.filename() == L"CMakeLists.txt" ? Language::Config : languageFor(utf8(path.extension().native()));
    indexDocument(*document); result->document = std::move(document); return result;
}
std::wstring trimmed(std::string text) { while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back(); return wide(text); }
std::shared_ptr<GitResult> readGit(const fs::path& directory, const std::atomic<bool>* cancelled) {
    auto result = std::make_shared<GitResult>();
    auto root = runProcess({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--show-toplevel"}, directory, 5000, cancelled);
    if (root.code) { if (root.code != 128) result->error = root.error; return result; }
    result->root = fs::path(trimmed(root.output));
    auto branch = runProcess({L"git.exe", L"--no-optional-locks", L"symbolic-ref", L"--short", L"-q", L"HEAD"}, result->root, 5000, cancelled);
    if (branch.code == 0) result->branch = trimmed(branch.output);
    else {
        auto head = runProcess({L"git.exe", L"--no-optional-locks", L"rev-parse", L"--short", L"HEAD"}, result->root, 5000, cancelled);
        result->branch = head.code == 0 ? trimmed(head.output) + L" (detached)" : L"HEAD";
    }
    auto status = runProcess({L"git.exe", L"--no-optional-locks", L"-c", L"core.fsmonitor=false", L"status", L"--porcelain=v1", L"-z", L"--untracked-files=all"}, result->root, 5000, cancelled);
    if (status.code) { result->error = status.error; return result; }
    for (size_t p = 0; p < status.output.size();) {
        const size_t end = status.output.find('\0', p); if (end == std::string::npos) break;
        const auto item = std::string_view(status.output).substr(p, end - p); p = end + 1;
        if (item.size() < 4) continue;
        const char x = item[0], y = item[1];
        const wchar_t state = x == '?' ? L'?' : x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D') ? L'U' : y != ' ' ? y : x;
        if (state == L'?') ++result->untracked; else ++result->changed;
        fs::path path = (result->root / fs::u8path(item.substr(3))).lexically_normal();
        if (result->entries.size() < maxTreeNodes) result->entries.push_back({path, state});
        else result->truncated = true;
        auto mark = [&](fs::path current) {
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
        if (x == 'R' || y == 'R' || x == 'C' || y == 'C') {
            const size_t sourceEnd = status.output.find('\0', p);
            if (sourceEnd == std::string::npos) break;
            mark(result->root / fs::u8path(status.output.substr(p, sourceEnd - p))); p = sourceEnd + 1;
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
    void openGitFile(TreeItem item);
    void chooseFolder();
    void openFolder(fs::path path);
    ProjectSession saveSession();
    void restoreProject(const ProjectSession& state);
    void forgetExpanded(const fs::path& path);
    void requestDirectory(TreeItem item);
    void applyDirectory(TreeItem item, Node* node, const DirectoryResult& result);
    TreeItem insert(TreeItem parent, const Entry& entry);
    Node* node(TreeItem item) const;
    void clearChildren(TreeItem parent);
    void updateLabel(TreeItem item);
    wchar_t stateFor(const fs::path& path) const;
    void openFile(const fs::path& path, bool diff = false);
    void saveTab();
    void selectTab(size_t index);
    void closeTab(size_t index);
    void toggleTabPin(size_t index);
    void tabMenu(POINT screen);
    void loadTab(bool preserve = false);
    void updateTabs(bool reveal = true);
    size_t tabEnd() const;
    bool tabOverflow() const { return tabRowStarts_.size() > tabRows_; }
    RECT tabBounds(size_t index) const;
    size_t tabAt(POINT point) const;
    std::optional<TabDrop> tabDropAt(POINT point) const;
    void updateTabDrag(POINT point);
    void scrollTabDrag();
    void endTabDrag(bool drop = false);
    fs::path selectedPath() const { return tabs_.empty() ? fs::path{} : tabs_[activeTab_].path; }
    void showDiff();
    void refresh(bool force = true);
    void refreshGit();
    void watch();
    void armWatch();
    void pollWatch();
    void stopWatch();
    void status(std::wstring message);
    void toggleTerminal();
    HWND window_ = nullptr, tree_ = nullptr, gitTree_ = nullptr, explorerButton_ = nullptr, gitButton_ = nullptr, tooltips_ = nullptr;
    HINSTANCE instance_ = nullptr;
    Tree explorer_, changes_;
    Renderer renderer_;
    ActivityButton explorerActivity_, gitActivity_;
    UINT dpi_ = 96;
    int sidebar_ = 245, terminalHeight_ = 260, toolbar_ = 44, footer_ = 26, splitter_ = 5;
    int activityWidth_ = 48, sidebarHeader_ = 34, gitHeader_ = 84;
    RECT sidebarRect_{}, sidebarSplitter_{}, terminalSplitter_{}, tabsRect_{};
    int dragging_ = 0, dragOffset_ = 0;
    bool terminalVisible_ = true;
    bool updatingGitFiles_ = false;
    bool gitTreeView_ = false;
    std::vector<GitChangeNode> gitChangeNodes_;
    std::set<std::wstring> collapsedGitPaths_;
    Sidebar activeSidebar_ = Sidebar::Explorer;
    Viewer viewer_;
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
    fs::path root_;
    std::map<std::wstring, fs::path> expandedPaths_;
    fs::path pendingTreeSelection_;
    bool waitingForGit_ = false;
    std::vector<Tab> tabs_;
    std::vector<RECT> tabRects_;
    std::vector<size_t> tabRowStarts_;
    size_t activeTab_ = 0, firstTab_ = 0, tabTools_ = 0;
    size_t firstTabRow_ = 0, tabRows_ = 0;
    std::optional<TabDrag> tabDrag_;
    TreeItem rootItem_ = 0;
    uint64_t generation_ = 0, serial_ = 0, nextId_ = 1;
    std::map<uint64_t, TreeItem> nodes_;
    std::shared_ptr<GitResult> git_;
    std::wstring status_ = L"Ready  ·  Ctrl+O: open folder";
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
    changes_.activated = [this](TreeItem item) { openGitFile(item); };
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
    explorerActivity_.clicked = [this] { selectSidebar(Sidebar::Explorer); };
    gitActivity_.clicked = [this] { selectSidebar(Sidebar::Git); };
    explorerActivity_.select(true);
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    for (const auto button : {explorerButton_, gitButton_}) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tip.hwnd = window_; tip.uId = reinterpret_cast<UINT_PTR>(button);
        tip.lpszText = const_cast<wchar_t*>(button == explorerButton_ ? L"Explorer (Ctrl+Shift+E)" : L"Source Control (Ctrl+Shift+G)");
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    viewer_.create(window_, instance_); terminal_.create(window_, instance_);
    terminal_.changed = [this] { if (owner_ && owner_->changed) owner_->changed(); };
    quickOpen_.create(window_, instance_);
    quickOpen_.opened = [this](const fs::path& path) { openFile(path); SetFocus(viewer_.window()); };
    // Each underlying pane must clip the palette when repainting beneath it.
    for (const auto pane : {tree_, gitTree_, viewer_.window(), terminal_.container(), explorerButton_, gitButton_})
        SetWindowLongPtrW(pane, GWL_STYLE, GetWindowLongPtrW(pane, GWL_STYLE) | WS_CLIPSIBLINGS);
    const BOOL dark = TRUE; DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    dpi(GetDpiForWindow(window_));
}
void ProjectRuntime::Impl::dpi(UINT value) {
    dpi_ = value; renderer_.setDpi(dpi_);
    explorer_.setDpi(dpi_); changes_.setDpi(dpi_, 28);
    explorerActivity_.setDpi(dpi_); gitActivity_.setDpi(dpi_);
    toolbar_ = MulDiv(44, dpi_, 96); footer_ = MulDiv(26, dpi_, 96); splitter_ = MulDiv(5, dpi_, 96);
    activityWidth_ = MulDiv(48, dpi_, 96); sidebarHeader_ = MulDiv(34, dpi_, 96); gitHeader_ = MulDiv(84, dpi_, 96);
    viewer_.setDpi(dpi_); terminal_.setDpi(dpi_); quickOpen_.setDpi(dpi_); layout();
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
    const int rowHeight = MulDiv(34, dpi_, 96), tabAreaWidth = std::max(0, width - right);
    const size_t maxRows = static_cast<size_t>(std::max(1, (viewerSize - MulDiv(100, dpi_, 96)) / rowHeight));
    std::vector<int> tabWidths;
    tabWidths.reserve(tabs_.size());
    for (const auto& tab : tabs_) {
        const auto title = tab.path.filename().native() + (tab.diff ? L" (diff)" : L"");
        tabWidths.push_back(renderer_.textWidth(title) + MulDiv(12, dpi_, 96) + MulDiv(28, dpi_, 96));
    }
    const auto wrapTabs = [&](int available) {
        tabRects_.clear(); tabRowStarts_.clear();
        int x = 0, y = 0;
        for (size_t i = 0; i < tabs_.size(); ++i) {
            const int tabWidth = std::min(tabWidths[i], available);
            // Pinned and regular tabs each start on their own row.
            const bool newGroup = i > 0 && tabs_[i].pinned != tabs_[i - 1].pinned;
            if (!i || newGroup || (x > 0 && tabWidth > available - x)) {
                tabRowStarts_.push_back(i);
                x = 0; y = static_cast<int>(tabRowStarts_.size() - 1) * rowHeight;
            }
            tabRects_.push_back({x, y, x + tabWidth, y + rowHeight});
            x += tabWidth;
        }
        tabRows_ = std::min(maxRows, tabRowStarts_.size());
    };
    wrapTabs(tabAreaWidth);
    // Keep the document usable even with many tabs. Only reserve navigation
    // buttons when all rows no longer fit above the document.
    if (tabOverflow()) wrapTabs(std::max(0, tabAreaWidth - MulDiv(56, dpi_, 96)));
    const int tabHeight = static_cast<int>(tabRows_) * rowHeight;
    tabsRect_ = {right, toolbar_, width, toolbar_ + tabHeight};
    updateTabs(revealTab);
    if (tabDrag_ && tabDrag_->active) tabDrag_->target = tabDropAt(tabDrag_->point);

    struct Placement { HWND window; int x, y, width, height, flags; };
    const std::array<Placement, 6> panes{{
        {explorerButton_, 0, toolbar_, activityWidth_ - 1, activityWidth_, SWP_SHOWWINDOW},
        {gitButton_, 0, toolbar_ + activityWidth_, activityWidth_ - 1, activityWidth_, SWP_SHOWWINDOW},
        {tree_, activityWidth_, toolbar_ + sidebarHeader_, side, std::max(0, content - sidebarHeader_), showExplorer ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {gitTree_, activityWidth_, gitTop, side, std::max(0, toolbar_ + content - gitTop), showGitFiles ? SWP_SHOWWINDOW : SWP_HIDEWINDOW},
        {viewer_.window(), right, toolbar_ + tabHeight, std::max(0, width - right), std::max(0, viewerSize - tabHeight), SWP_SHOWWINDOW},
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
    dc.label({left, toolbar_, edge, top}, activeSidebar_ == Sidebar::Explorer ? L"EXPLORER" : L"SOURCE CONTROL", foreground);
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
            const std::wstring message = root_.empty() ? L"Open a folder to view Git changes." : !git_ ? L"Loading Git changes…" : !git_->error.empty() ? L"Git unavailable.\nCheck Git for Windows and press F5 to retry." : git_->root.empty() ? L"This folder is not in a Git repository." : L"No pending changes.";
            dc.label({left, top + gitHeader_ + scaled(16), edge, sidebarRect_.bottom - scaled(12)}, message, muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    } else if (root_.empty()) {
        dc.label({left, top + scaled(16), edge, sidebarRect_.bottom - scaled(12)}, L"Open a folder with Ctrl+O to browse files.", muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    if (!tabs_.empty()) {
        dc.fill(tabsRect_, background);
        for (size_t i = firstTab_; i < tabEnd(); ++i) {
            const auto rect = tabBounds(i);
            const bool active = i == activeTab_;
            dc.fill(rect, active ? panel : background);
            if (active) dc.fill({rect.left, rect.top, rect.right, rect.top + scaled(2)}, accent);
            const auto title = tabs_[i].path.filename().native() + (tabs_[i].diff ? L" (diff)" : L"");
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
        if (tabOverflow()) {
            const int edge = tabsRect_.right, button = scaled(28), top = tabsRect_.bottom - scaled(34);
            dc.label({edge - 2 * button, top, edge - button, tabsRect_.bottom}, L"‹", firstTab_ ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            dc.label({edge - button, top, edge, tabsRect_.bottom}, L"›", tabEnd() < tabs_.size() ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        for (size_t row = 1; row <= tabRows_; ++row) {
            const int bottom = tabsRect_.top + static_cast<int>(row) * scaled(34);
            dc.fill({tabsRect_.left, bottom - 1, tabsRect_.right, bottom}, border);
        }
        if (tabDrag_ && tabDrag_->active && tabDrag_->target) dc.fill(tabDrag_->target->marker, accent);
    }
}
void ProjectRuntime::Impl::selectSidebar(Sidebar sidebar) {
    activeSidebar_ = sidebar; layout();
    explorerActivity_.select(sidebar == Sidebar::Explorer); gitActivity_.select(sidebar == Sidebar::Git);
    SetFocus(sidebar == Sidebar::Explorer ? (IsWindowVisible(tree_) ? tree_ : explorerButton_) : (IsWindowVisible(gitTree_) ? gitTree_ : gitButton_));
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
    if (const auto node = changes_.item(changes_.selection()); node && node->data < gitChangeNodes_.size())
        selected = gitChangeNodes_[node->data].path;
    updatingGitFiles_ = true;
    {
        Tree::Batch batch(changes_);
        changes_.clear();
        gitChangeNodes_.clear();
        TreeItem selection = 0;
        if (git_ && git_->error.empty()) {
            std::vector<std::wstring> paths; paths.reserve(git_->entries.size());
            for (const auto& entry : git_->entries) paths.push_back(entry.path.lexically_relative(git_->root).generic_wstring());
            gitChangeNodes_ = buildGitChanges(paths, gitTreeView_);
            std::vector<TreeItem> items; items.reserve(gitChangeNodes_.size());
            TreeItem active = 0;
            const auto activePath = pathKey(selectedPath());
            for (size_t i = 0; i < gitChangeNodes_.size(); ++i) {
                const auto& node = gitChangeNodes_[i];
                const auto parent = node.parent == noGitChange ? 0 : items[node.parent];
                const auto offset = node.parent == noGitChange ? 0 : gitChangeNodes_[node.parent].path.size() + 1;
                auto text = node.path.substr(offset);
                COLORREF color = foreground;
                if (!node.directory()) {
                    const auto& entry = git_->entries[node.entry];
                    if (!gitTreeView_) text = entry.path.lexically_relative(root_).native();
                    color = stateColor(entry.state);
                }
                const auto item = changes_.insert(parent, std::move(text), node.directory(), i, color);
                items.push_back(item);
                if (node.directory()) changes_.expand(item, !collapsedGitPaths_.count(node.path));
                if (node.path == selected) selection = item;
                if (!node.directory() && pathKey(git_->entries[node.entry].path) == activePath) active = item;
            }
            if (!selection) selection = active;
            // Restore a visible ancestor when the selected file is folded away.
            for (auto item = selection; item;) {
                const auto node = changes_.item(item);
                item = node ? node->parent : 0;
                if (const auto parent = changes_.item(item); parent && !parent->expanded) selection = item;
            }
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
    if (node.directory() || node.entry >= git_->entries.size()) return;
    const auto entry = git_->entries[node.entry];
    openFile(entry.path, entry.state != L'?');
}
void ProjectRuntime::Impl::status(std::wstring message) { status_ = std::move(message); RECT r{}; GetClientRect(window_, &r); r.top = r.bottom - footer_; InvalidateRect(window_, &r, FALSE); }
void ProjectRuntime::Impl::chooseFolder() {
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
wchar_t ProjectRuntime::Impl::stateFor(const fs::path& path) const {
    if (!git_) return 0;
    const auto direct = git_->states.find(pathKey(path));
    return direct != git_->states.end() ? direct->second : 0;
}
void ProjectRuntime::Impl::updateLabel(TreeItem item) {
    const auto n = node(item); if (!n) return;
    std::wstring text = n->path.filename().empty() ? n->path.native() : n->path.filename().native();
    if (n->directory && n->reparse) text += L"  [link]";
    const auto state = stateFor(n->path);
    explorer_.label(item, std::move(text), state ? stateColor(state) : foreground);
}
TreeItem ProjectRuntime::Impl::insert(TreeItem parent, const Entry& entry) {
    if (nodes_.size() >= maxTreeNodes) return 0;
    auto n = std::make_unique<Node>(); n->path = entry.path; n->id = nextId_++; n->directory = entry.directory; n->reparse = entry.reparse;
    const auto item = explorer_.insert(parent, L"", n->directory && !n->reparse, reinterpret_cast<uintptr_t>(n.get()));
    if (item) { nodes_[n->id] = item; n.release(); updateLabel(item); }
    return item;
}
void ProjectRuntime::Impl::openFolder(fs::path path) {
    quickOpen_.close();
    endTabDrag();
    root_ = std::move(path);
    auto saved = std::move(initialState_);
    restoreProject(saved.value_or(ProjectSession{}));
    if (!saved) expandedPaths_[pathKey(root_)] = root_;
    viewer_.message(L"Select a file on the left.\n\nFiles are read only. Your AI CLI can edit them from the terminal.");
    updateGitFiles();
    rootItem_ = insert(0, {root_, true, false});
    if (expandedPaths_.count(pathKey(root_))) requestDirectory(rootItem_);
    if (pathKey(pendingTreeSelection_) == pathKey(root_)) { explorer_.select(rootItem_, false); pendingTreeSelection_.clear(); }
    terminal_.setDirectory(root_); watch(); quickOpen_.setRoot(root_); refreshGit();
    SetWindowTextW(window_, (L"Poppy — " + root_.native()).c_str()); status(L"F5: refresh  ·  F6: diff  ·  Ctrl+`: terminal"); InvalidateRect(window_, nullptr, FALSE);
    if (!tabs_.empty()) loadTab();
}
ProjectSession ProjectRuntime::Impl::saveSession() {
    saveTab();
    ProjectSession state;
    state.sidebar = sidebar_; state.terminalHeight = terminalHeight_; state.terminalVisible = terminalVisible_;
    state.gitSidebar = activeSidebar_ == Sidebar::Git; state.activeTab = activeTab_;
    state.diffInline = viewer_.diffInline(); state.gitTree = gitTreeView_; state.firstTabRow = firstTabRow_;
    for (const auto& tab : tabs_) state.tabs.push_back({tab.path.u8string(), tab.diff, tab.fileView, tab.diffView, tab.pinned});
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
    else if (focus == viewer_.window()) focusedPane_ = "viewer";
    state.focusedPane = focusedPane_;
    terminal_.snapshot(state);
    return state;
}
void ProjectRuntime::Impl::restoreProject(const ProjectSession& state) {
    focusedPane_ = state.focusedPane;
    firstTabRow_ = state.firstTabRow;
    restoreExplorerTop_ = state.explorerTop; restoreExplorerLeft_ = state.explorerLeft;
    restoreGitTop_ = state.gitTop; restoreGitLeft_ = state.gitLeft; pendingGitSelection_ = state.gitSelected;
    for (const auto& path : state.collapsedGit) collapsedGitPaths_.insert(wide(path));
    terminal_.setDirectory(root_); terminal_.restore(state);
    viewer_.setDiffLayout(state.diffInline);
    gitTreeView_ = state.gitTree;
    sidebar_ = state.sidebar; terminalHeight_ = state.terminalHeight; terminalVisible_ = state.terminalVisible;
    activeSidebar_ = state.gitSidebar ? Sidebar::Git : Sidebar::Explorer;
    explorerActivity_.select(!state.gitSidebar); gitActivity_.select(state.gitSidebar);
    const auto rootKey = pathKey(root_);
    for (const auto& path : state.expanded) {
        auto directory = fs::u8path(path).lexically_normal().make_preferred();
        const auto key = pathKey(directory);
        if (key == rootKey || (key.size() > rootKey.size() && key.compare(0, rootKey.size(), rootKey) == 0 &&
            (rootKey.back() == L'\\' || key[rootKey.size()] == L'\\'))) expandedPaths_[key] = std::move(directory);
    }
    pendingTreeSelection_ = fs::u8path(state.selected);
    for (size_t i = 0; i < state.tabs.size(); ++i) {
        const auto& saved = state.tabs[i];
        const auto path = fs::u8path(saved.path);
        if (!path.is_absolute()) continue;
        const auto duplicate = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return pathKey(tab.path) == pathKey(path); });
        if (duplicate != tabs_.end()) continue;
        if (i <= state.activeTab) activeTab_ = tabs_.size();
        tabs_.push_back({path, saved.diff, saved.fileView, saved.diffView, saved.pinned});
    }
}
void ProjectRuntime::Impl::forgetExpanded(const fs::path& path) {
    const auto key = pathKey(path);
    for (auto iterator = expandedPaths_.begin(); iterator != expandedPaths_.end();) {
        const auto& candidate = iterator->first;
        if (candidate == key || (candidate.size() > key.size() && candidate.compare(0, key.size(), key) == 0 &&
            (key.back() == L'\\' || candidate[key.size()] == L'\\'))) iterator = expandedPaths_.erase(iterator);
        else ++iterator;
    }
}
void ProjectRuntime::Impl::requestDirectory(TreeItem item) {
    const auto n = node(item); if (!n || !n->directory || n->reparse) return;
    n->expandRequested = true;
    expandedPaths_[pathKey(n->path)] = n->path;
    if (n->loading) return;
    n->loading = true; const auto path = n->path; const auto id = n->id, generation = generation_;
    worker_->submit(L"dir:" + std::to_wstring(id), [this, path, id, generation] {
        auto result = listDirectory(path);
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
    for (auto child = explorer_.child(item); child; child = explorer_.next(child)) if (const auto n = node(child)) existing[pathKey(n->path)] = child;
    Tree::Batch batch(explorer_);
    for (const auto& entry : result.entries) {
        const auto found = existing.find(pathKey(entry.path));
        if (found != existing.end()) {
            auto n = node(found->second);
            if (n && n->directory == entry.directory && n->reparse == entry.reparse) { existing.erase(found); continue; }
            if (n) forgetExpanded(n->path);
            explorer_.erase(found->second); existing.erase(found);
        }
        insert(item, entry);
    }
    for (const auto& stale : existing) { if (const auto n = node(stale.second)) forgetExpanded(n->path); explorer_.erase(stale.second); }
    explorer_.sort(item, [](uintptr_t a, uintptr_t b) {
        const auto x = reinterpret_cast<Node*>(a), y = reinterpret_cast<Node*>(b);
        if (x->directory != y->directory) return x->directory;
        return CompareStringOrdinal(x->path.filename().c_str(), -1, y->path.filename().c_str(), -1, TRUE) == CSTR_LESS_THAN;
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
size_t ProjectRuntime::Impl::tabEnd() const {
    const size_t endRow = firstTabRow_ + tabRows_;
    return endRow < tabRowStarts_.size() ? tabRowStarts_[endRow] : tabs_.size();
}
RECT ProjectRuntime::Impl::tabBounds(size_t index) const {
    auto rect = tabRects_[index];
    OffsetRect(&rect, tabsRect_.left, tabsRect_.top - static_cast<int>(firstTabRow_) * MulDiv(34, dpi_, 96));
    return rect;
}
size_t ProjectRuntime::Impl::tabAt(POINT point) const {
    if (PtInRect(&tabsRect_, point)) {
        for (size_t i = firstTab_; i < tabEnd(); ++i) {
            const auto rect = tabBounds(i);
            if (PtInRect(&rect, point)) return i;
        }
    }
    return tabs_.size();
}
std::optional<TabDrop> ProjectRuntime::Impl::tabDropAt(POINT point) const {
    if (!tabDrag_ || tabDrag_->index >= tabs_.size() || !PtInRect(&tabsRect_, point)) return std::nullopt;
    if (tabOverflow() && point.x >= tabsRect_.right - MulDiv(56, dpi_, 96) &&
        point.y >= tabsRect_.bottom - MulDiv(34, dpi_, 96)) return std::nullopt;
    const int stroke = std::max(2, MulDiv(3, dpi_, 96)), inset = MulDiv(3, dpi_, 96);
    const auto target = [&](size_t position, const RECT& rect, LONG x) {
        const LONG left = std::clamp<LONG>(x - stroke / 2, tabsRect_.left, std::max(tabsRect_.left, tabsRect_.right - stroke));
        return TabDrop{position, {left, rect.top + inset, left + stroke, rect.bottom - inset}};
    };
    std::optional<TabDrop> result;
    for (size_t i = firstTab_; i < tabEnd(); ++i) {
        const auto rect = tabBounds(i);
        if (point.y < rect.top || point.y >= rect.bottom || tabs_[i].pinned != tabs_[tabDrag_->index].pinned) continue;
        if (point.x < rect.left + (rect.right - rect.left) / 2) return target(i, rect, rect.left);
        result = target(i + 1, rect, rect.right);
    }
    return result;
}
void ProjectRuntime::Impl::updateTabDrag(POINT point) {
    if (!tabDrag_) return;
    auto& drag = *tabDrag_;
    drag.point = point;
    if (!drag.active) {
        if (std::abs(point.x - drag.origin.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi_) &&
            std::abs(point.y - drag.origin.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi_)) return;
        drag.active = true;
        SendMessageW(tooltips_, TTM_ACTIVATE, FALSE, 0);
        SetTimer(window_, tabDragTimerId, 300, nullptr);
    }
    drag.target = tabDropAt(point);
    InvalidateRect(window_, &tabsRect_, FALSE);
}
void ProjectRuntime::Impl::scrollTabDrag() {
    if (!tabDrag_ || !tabDrag_->active || !tabOverflow()) return;
    const auto point = tabDrag_->point;
    if (!PtInRect(&tabsRect_, point) || point.y < tabsRect_.bottom - MulDiv(34, dpi_, 96)) return;
    const int button = MulDiv(28, dpi_, 96);
    if (point.x >= tabsRect_.right - button && tabEnd() < tabs_.size()) ++firstTabRow_;
    else if (point.x >= tabsRect_.right - 2 * button && point.x < tabsRect_.right - button && firstTabRow_) --firstTabRow_;
    else return;
    updateTabs(false);
    tabDrag_->target = tabDropAt(point);
}
void ProjectRuntime::Impl::endTabDrag(bool drop) {
    if (!tabDrag_) return;
    const auto drag = *tabDrag_;
    tabDrag_.reset();
    KillTimer(window_, tabDragTimerId);
    if (GetCapture() == window_) ReleaseCapture();
    if (!drag.active) return;
    SendMessageW(tooltips_, TTM_ACTIVATE, TRUE, 0);
    if (drop && drag.target) {
        const size_t from = drag.index, position = drag.target->position;
        const size_t to = position > from ? position - 1 : position;
        if (from != to) {
            auto tab = std::move(tabs_[from]);
            tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
            tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(to), std::move(tab));
            if (activeTab_ == from) activeTab_ = to;
            else if (from < activeTab_ && activeTab_ <= to) --activeTab_;
            else if (to <= activeTab_ && activeTab_ < from) ++activeTab_;
            layout();
            return;
        }
    }
    // Reveal the selected tab again after scrolling or cancelling a drag.
    updateTabs();
}
void ProjectRuntime::Impl::updateTabs(bool reveal) {
    const size_t lastFirst = tabRowStarts_.size() - tabRows_;
    firstTabRow_ = std::min(firstTabRow_, lastFirst);
    if (reveal && !tabs_.empty()) {
        const size_t activeRow = static_cast<size_t>(std::upper_bound(tabRowStarts_.begin(), tabRowStarts_.end(), activeTab_) - tabRowStarts_.begin() - 1);
        if (activeRow < firstTabRow_) firstTabRow_ = activeRow;
        else if (activeRow >= firstTabRow_ + tabRows_) firstTabRow_ = activeRow - tabRows_ + 1;
    }
    firstTab_ = tabRowStarts_.empty() ? 0 : tabRowStarts_[firstTabRow_];
    // Only visible tabs need tooltip regions. Resolve their paths on demand.
    for (size_t i = 0; i < tabTools_; ++i) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.hwnd = window_; tip.uId = 100 + i;
        SendMessageW(tooltips_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    tabTools_ = 0;
    for (size_t i = firstTab_; i < tabEnd(); ++i) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_SUBCLASS; tip.hwnd = window_; tip.uId = 100 + tabTools_++;
        tip.rect = tabBounds(i); tip.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    InvalidateRect(window_, &tabsRect_, FALSE);
}
void ProjectRuntime::Impl::saveTab() {
    if (!tabs_.empty() && viewer_.hasDocument()) {
        auto& tab = tabs_[activeTab_];
        (tab.diff ? tab.diffView : tab.fileView) = viewer_.state();
    }
}
void ProjectRuntime::Impl::openFile(const fs::path& path, bool diff) {
    endTabDrag();
    const auto key = pathKey(path);
    const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return pathKey(tab.path) == key; });
    const size_t index = static_cast<size_t>(found - tabs_.begin());
    if (found != tabs_.end() && index == activeTab_ && (!diff || found->diff)) { updateTabs(); return; }
    saveTab();
    if (found == tabs_.end()) { Tab tab; tab.path = path; tab.diff = diff; tabs_.push_back(std::move(tab)); }
    else if (diff) found->diff = true;
    activeTab_ = index; layout(); loadTab();
}
void ProjectRuntime::Impl::selectTab(size_t index) {
    endTabDrag();
    if (index >= tabs_.size() || index == activeTab_) return;
    saveTab(); activeTab_ = index; updateTabs(); loadTab();
}
void ProjectRuntime::Impl::closeTab(size_t index) {
    endTabDrag();
    if (index >= tabs_.size() || tabs_[index].pinned) return;
    saveTab(); const bool active = index == activeTab_;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < activeTab_) --activeTab_;
    else if (activeTab_ >= tabs_.size()) activeTab_ = tabs_.empty() ? 0 : tabs_.size() - 1;
    layout();
    if (active) {
        ++serial_;
        if (!tabs_.empty()) loadTab();
        else { viewer_.message(L"Select a file on the left.\n\nFiles are read only. Your AI CLI can edit them from the terminal."); status(L"Ready"); }
    }
    SetFocus(viewer_.window());
}
void ProjectRuntime::Impl::toggleTabPin(size_t index) {
    endTabDrag();
    if (index >= tabs_.size()) return;
    saveTab();
    const auto activePath = selectedPath();
    auto tab = std::move(tabs_[index]);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    tab.pinned = !tab.pinned;
    const auto position = std::find_if(tabs_.begin(), tabs_.end(), [](const Tab& value) { return !value.pinned; });
    tabs_.insert(position, std::move(tab));
    activeTab_ = static_cast<size_t>(std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& value) { return value.path == activePath; }) - tabs_.begin());
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
    const bool pinned = tabs_[index].pinned;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, pinned ? L"Unpin tab" : L"Pin tab");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (pinned ? MF_GRAYED : 0), 2, L"Close tab");
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(menu);
    const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tab.path == path; });
    if (found != tabs_.end()) {
        index = static_cast<size_t>(found - tabs_.begin());
        if (command == 1) toggleTabPin(index);
        else if (command == 2) closeTab(index);
    }
}
void ProjectRuntime::Impl::loadTab(bool preserve) {
    waitingForGit_ = false;
    if (tabs_.empty()) return;
    preserve = preserve && viewer_.hasDocument();
    const auto& tab = tabs_[activeTab_]; const auto path = tab.path;
    const auto state = tab.diff ? tab.diffView : tab.fileView;
    const auto serial = ++serial_, generation = generation_;
    // Retain only tab metadata and view positions; reload the active document.
    if (!preserve) viewer_.message(tab.diff ? L"Loading Git diff…" : L"Loading…", path);
    status((tab.diff ? L"Git diff: " : L"Loading ") + path.filename().native());
    if (!tab.diff) {
        worker_->submit(L"file", [this, path, serial, generation, preserve, state] {
            auto result = readDocument(path);
            worker_->complete([this, result, path, serial, generation, preserve, state] {
                if (generation != generation_ || serial != serial_) return;
                if (result->document) {
                    viewer_.show(result->document, path, preserve); if (!preserve) viewer_.restore(state);
                    status(std::to_wstring(result->document->lines.size()) + L" lines  ·  read only  ·  F6: diff");
                } else { viewer_.message(result->error, path); status(L"Preview unavailable"); }
            });
        });
        return;
    }
    if (!git_) { waitingForGit_ = true; viewer_.message(L"Loading Git repository…", path); return; }
    if (git_->root.empty() || !git_->error.empty()) { viewer_.message(L"Git repository unavailable. Refresh after installing Git for Windows.", path); return; }
    const auto root = git_->root;
    gitWorker_->submit(L"diff", [this, serial, generation, path, root, preserve, state] {
        const auto relative = path.lexically_relative(root).native();
        auto readDiff = [&](bool staged) {
            std::vector<std::wstring> args{L"git.exe", L"-c", L"diff.suppressBlankEmpty=false", L"--no-optional-locks", L"diff", L"--patch",
                L"--no-ext-diff", L"--no-textconv", L"--color=never",
                L"--output-indicator-old=-", L"--output-indicator-new=+", L"--output-indicator-context= ",
                staged ? L"--cached" : L"--ours", L"--", relative};
            return runProcess(args, root, 5000, gitWorker_->cancellation());
        };
        auto result = readDiff(false);
        bool staged = false;
        if (result.code == 0 && result.output.empty()) { staged = true; result = readDiff(true); }
        auto doc = std::make_shared<DiffDocument>();
        if (result.code == 0 && static_cast<size_t>(std::count(result.output.begin(), result.output.end(), '\n')) >= maxLines) { result.code = 1; result.error = L"Diff exceeds the 1,000,000-line preview limit."; }
        if (result.code == 0) { doc->patch.text = std::move(result.output); indexDiff(*doc); }
        const auto error = result.error; const auto code = result.code;
        gitWorker_->complete([this, serial, generation, path, doc, code, error, staged, preserve, state] {
            if (generation != generation_ || serial != serial_) return;
            if (code) viewer_.message(error, path);
            else if (doc->patch.text.empty()) viewer_.message(L"No tracked changes for this file.\nUntracked files can be read in the normal preview.\n\nPress F6 to return to the file.", path);
            else if (doc->rows.empty()) viewer_.message(L"No text changes to display.\n\nPress F6 to return to the file.", path);
            else { viewer_.showDiff(doc, path, preserve); if (!preserve) viewer_.restore(state); }
            status(staged ? L"Staged diff  ·  F6: return to file  ·  F7: split / inline" : L"Working tree diff  ·  F6: return to file  ·  F7: split / inline");
        });
    });
}
void ProjectRuntime::Impl::showDiff() {
    if (tabs_.empty()) { status(L"Select a file to view its Git diff."); return; }
    auto& tab = tabs_[activeTab_];
    if (!tab.diff && (!git_ || git_->root.empty() || !git_->error.empty())) { status(L"Git repository unavailable. Refresh after installing Git for Windows."); return; }
    saveTab(); tab.diff = !tab.diff; layout(); loadTab();
}
void ProjectRuntime::Impl::refreshGit() {
    if (root_.empty()) return;
    const auto generation = generation_; const auto root = root_;
    gitWorker_->submit(L"git", [this, root, generation] {
        auto result = readGit(root, gitWorker_->cancellation());
        gitWorker_->complete([this, generation, result] {
            if (generation != generation_) return;
            git_ = result;
            { Tree::Batch batch(explorer_); for (const auto& pair : nodes_) updateLabel(pair.second); }
            updateGitFiles();
            if (waitingForGit_) loadTab();
            if (!result->error.empty()) status(L"Git: " + result->error);
            else if (result->truncated) status(L"Git display limited to 50,000 files and badge paths.");
            InvalidateRect(window_, nullptr, FALSE);
        });
    });
}
void ProjectRuntime::Impl::refresh(bool force) {
    if (root_.empty()) return;
    if (force || filesDirty_) quickOpen_.refresh();
    filesDirty_ = false;
    for (const auto& pair : nodes_) { const auto n = node(pair.second); if (n && n->expandRequested) requestDirectory(pair.second); }
    refreshGit();
    if (!tabs_.empty() && (force || fileDirty_)) {
        if (!IsWindowVisible(window_) && !viewer_.hasDocument()) { fileDirty_ = true; return; }
        saveTab(); loadTab(true);
    }
    fileDirty_ = false;
}
void ProjectRuntime::Impl::watch() {
    watchDirectory_.reset(CreateFileW(root_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
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
                if (changed == selected || selected.rfind(changed + L"\\", 0) == 0) fileDirty_ = true;
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
    terminalVisible_ = !terminalVisible_; layout(); SetFocus(terminalVisible_ ? terminal_.window() : viewer_.window());
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
    case WM_APP + 13: if (!root_.empty()) { if (!watchDirectory_) watch(); refresh(); } return 0;
    case WM_APP + 12: if (!root_.empty() && IsWindowVisible(window_)) { refreshGit(); if (!tabs_.empty()) { saveTab(); loadTab(true); } } return 0;
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
                    selectTab(index); SetFocus(viewer_.window());
                    tabDrag_ = TabDrag{index, point, point, false, std::nullopt};
                    SetCapture(window_);
                }
            } else if (tabOverflow() && point.y >= tabsRect_.bottom - MulDiv(34, dpi_, 96)) {
                const int button = MulDiv(28, dpi_, 96);
                if (point.x >= tabsRect_.right - button && tabEnd() < tabs_.size()) ++firstTabRow_;
                else if (point.x >= tabsRect_.right - 2 * button && point.x < tabsRect_.right - button && firstTabRow_) --firstTabRow_;
                updateTabs(false);
            }
            return 0;
        }
        if (PtInRect(&sidebarSplitter_, point)) { dragging_ = 1; dragOffset_ = point.x - sidebarSplitter_.left; }
        else if (terminalVisible_ && PtInRect(&terminalSplitter_, point)) dragging_ = 2;
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
            if (PtInRect(&sidebarSplitter_, p)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            if (terminalVisible_ && PtInRect(&terminalSplitter_, p)) { SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
        }
        break;
    case WM_NOTIFY: {
        const auto header = reinterpret_cast<NMHDR*>(b);
        if (header->hwndFrom == tooltips_ && header->code == TTN_GETDISPINFOW && header->idFrom >= 100 && header->idFrom - 100 < tabTools_) {
            const size_t index = firstTab_ + header->idFrom - 100;
            if (index < tabs_.size()) reinterpret_cast<NMTTDISPINFOW*>(b)->lpszText = const_cast<wchar_t*>(tabs_[index].path.c_str());
            return 0;
        }
        return 0;
    }
    case WM_CLOSE: return 0;
    case WM_DESTROY:
        KillTimer(window_, timerId); stopWatch(); quickOpen_.stop(); if (gitWorker_) gitWorker_->stop(); if (worker_) worker_->stop(); terminal_.stop(); explorer_.clear(); changes_.clear();
        return 0;
    default: break;
    }
    return DefWindowProcW(window_, m, a, b);
}
bool ProjectRuntime::Impl::key(const MSG& message) {
        const bool key = message.message == WM_KEYDOWN;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (key && message.wParam == VK_ESCAPE && tabDrag_) { endTabDrag(); return true; }
        if (key && ctrl && !shift && !(GetKeyState(VK_MENU) & 0x8000) && message.wParam == 'P') { quickOpen_.show(); return true; }
        if (key && ctrl && message.wParam == 'O') { quickOpen_.close(); chooseFolder(); return true; }
        if (quickOpen_.visible() && key) return false;
        if (key && ctrl && shift && message.wParam == 'E') { selectSidebar(Sidebar::Explorer); return true; }
        if (key && ctrl && shift && message.wParam == 'G') { selectSidebar(Sidebar::Git); return true; }
        if (key && ctrl && message.wParam == VK_OEM_3) { toggleTerminal(); return true; }
        if (key && ctrl && message.wParam == VK_TAB && !tabs_.empty()) { selectTab((activeTab_ + (shift ? tabs_.size() - 1 : 1)) % tabs_.size()); SetFocus(viewer_.window()); return true; }
        if (key && ctrl && message.wParam == 'W' && GetFocus() != terminal_.window()) { closeTab(activeTab_); return true; }
        if (key && message.wParam == VK_F5 && GetFocus() != terminal_.window()) { refresh(); return true; }
        if (key && message.wParam == VK_F6 && GetFocus() != terminal_.window()) { showDiff(); return true; }
        if (key && message.wParam == VK_F7 && GetFocus() != terminal_.window()) { viewer_.toggleDiffLayout(); return true; }
        if (key && message.wParam == VK_TAB && GetFocus() != terminal_.window()) {
            const HWND files = activeSidebar_ == Sidebar::Explorer ? (IsWindowVisible(tree_) ? tree_ : explorerButton_) : (IsWindowVisible(gitTree_) ? gitTree_ : gitButton_);
            SetFocus(GetFocus() == files ? viewer_.window() : terminalVisible_ ? terminal_.window() : files); return true;
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
void ProjectRuntime::revalidate() { PostMessageW(window(), WM_APP+13, 0, 0); }
void ProjectRuntime::initialize(const fs::path& path, const ProjectSession& state, bool saved) { if (saved) impl_->initialState_ = state; impl_->openFolder(path); }
void ProjectRuntime::activate(bool active) {
    if (!active) {
        impl_->saveTab(); impl_->lastFocus_ = GetFocus();
        if (impl_->lastFocus_ == impl_->terminal_.window()) impl_->focusedPane_ = "terminal";
        else if (impl_->lastFocus_ == impl_->tree_) impl_->focusedPane_ = "explorer";
        else if (impl_->lastFocus_ == impl_->gitTree_) impl_->focusedPane_ = "git";
        else if (impl_->lastFocus_ == impl_->viewer_.window()) impl_->focusedPane_ = "viewer";
        impl_->quickOpen_.close(false); impl_->endTabDrag(); ShowWindow(window(), SW_HIDE);
    }
    else {
        ShowWindow(window(), SW_SHOWNA); impl_->layout(false);
        HWND focus = impl_->lastFocus_;
        if (!focus || !IsWindow(focus) || !IsWindowVisible(focus)) {
            focus = impl_->focusedPane_ == "terminal" && impl_->terminalVisible_ ? impl_->terminal_.window() : impl_->focusedPane_ == "explorer" && IsWindowVisible(impl_->tree_) ? impl_->tree_ : impl_->focusedPane_ == "git" && IsWindowVisible(impl_->gitTree_) ? impl_->gitTree_ : impl_->viewer_.window();
        }
        SetFocus(focus); impl_->terminal_.acknowledge(); PostMessageW(window(), WM_APP+12, 0, 0);
    }
}
void ProjectRuntime::setDpi(UINT dpi) { impl_->dpi(dpi); }
void ProjectRuntime::poll() { if (ready()) impl_->pollWatch(); if (IsWindowVisible(impl_->terminal_.container())) { impl_->terminal_.acknowledge(); if (summary().counts[static_cast<size_t>(AiState::Running)]) InvalidateRect(impl_->terminal_.container(), nullptr, FALSE); } }
bool ProjectRuntime::key(const MSG& message) { return impl_->key(message); }
ProjectSession ProjectRuntime::snapshot() { return impl_->saveSession(); }
AiSummary ProjectRuntime::summary() const { return impl_->terminal_.summary(); }
std::wstring ProjectRuntime::description() const { return impl_->terminal_.description(); }
bool ProjectRuntime::apply(const AiEvent& event) { return impl_->terminal_.apply(event); }
void ProjectRuntime::focusAttention() { impl_->terminalVisible_ = true; impl_->layout(false); impl_->terminal_.focusAttention(); }
size_t ProjectRuntime::documentBytes() const { return impl_->viewer_.documentBytes(); }
void ProjectRuntime::releaseDocument() { impl_->saveTab(); ++impl_->serial_; impl_->waitingForGit_ = false; impl_->viewer_.message(L"Loading…"); }
HWND ProjectRuntime::window() const { return impl_->window_; }
bool ProjectRuntime::ready() const { return !impl_->root_.empty(); }

}
