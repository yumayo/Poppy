#include "viewer.h"
#include "terminal.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>
#include <map>
#include <memory>

namespace poppy {
namespace {
constexpr UINT_PTR timerId = 1;
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
struct Tab { fs::path path; bool diff = false; Viewer::State fileView, diffView; };
struct GitEntry { fs::path path; wchar_t state; };
struct GitResult { fs::path root; std::wstring branch, error; std::map<std::wstring, wchar_t> states; std::vector<GitEntry> entries; size_t changed = 0, untracked = 0; bool truncated = false; };

COLORREF stateColor(wchar_t state) {
    return state == L'U' || state == L'D' ? RGB(231,112,118) : state == L'?' || state == L'A' ? accent : RGB(224,192,128);
}

void activityIcon(Renderer& dc, const RECT& rect, bool git, COLORREF color, UINT dpi) {
    const int size = MulDiv(24, dpi, 96), x = (rect.left + rect.right - size) / 2, y = (rect.top + rect.bottom - size) / 2;
    auto px = [&](int value) { return x + MulDiv(value, dpi, 96); };
    auto py = [&](int value) { return y + MulDiv(value, dpi, 96); };
    const float stroke = static_cast<float>(std::max(1, MulDiv(2, dpi, 96)));
    auto line = [&](int x1, int y1, int x2, int y2) { dc.line(static_cast<float>(px(x1)), static_cast<float>(py(y1)), static_cast<float>(px(x2)), static_cast<float>(py(y2)), color, stroke); };
    if (git) {
        line(6, 6, 6, 18); line(18, 6, 18, 9); line(18, 9, 6, 15);
        dc.ellipse({px(3), py(1), px(9), py(7)}, color, stroke);
        dc.ellipse({px(15), py(1), px(21), py(7)}, color, stroke);
        dc.ellipse({px(3), py(17), px(9), py(23)}, color, stroke);
    } else {
        line(6, 17, 2, 17); line(2, 17, 2, 2); line(2, 2, 15, 2); line(15, 2, 15, 5);
        dc.outline({px(7), py(7), px(21), py(23)}, color, stroke);
    }
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

class App {
public:
    int run(HINSTANCE instance, int show, fs::path initial);
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void create();
    void dpi(UINT value);
    void layout(bool revealTab = true);
    void paint();
    void drawActivity(const DRAWITEMSTRUCT& item);
    void selectSidebar(Sidebar sidebar);
    void updateGitFiles();
    void openGitFile(HTREEITEM item);
    void chooseFolder();
    void openFolder(fs::path path);
    void requestDirectory(HTREEITEM item);
    void applyDirectory(HTREEITEM item, Node* node, const DirectoryResult& result);
    HTREEITEM insert(HTREEITEM parent, const Entry& entry);
    Node* node(HTREEITEM item) const;
    void clearChildren(HTREEITEM parent);
    void updateLabel(HTREEITEM item);
    wchar_t stateFor(const fs::path& path) const;
    void openFile(const fs::path& path, bool diff = false);
    void saveTab();
    void selectTab(size_t index);
    void closeTab(size_t index);
    void loadTab(bool preserve = false);
    void updateTabs(bool reveal = true);
    size_t tabCapacity() const;
    RECT tabBounds(size_t index) const;
    size_t tabAt(POINT point) const;
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
    HFONT uiFont_ = nullptr;
    Renderer renderer_, explorerRenderer_, gitRenderer_;
    UINT dpi_ = 96;
    int sidebar_ = 245, terminalHeight_ = 260, toolbar_ = 44, footer_ = 26, splitter_ = 5;
    int activityWidth_ = 48, sidebarHeader_ = 34, gitHeader_ = 56;
    RECT sidebarRect_{}, sidebarSplitter_{}, terminalSplitter_{}, tabsRect_{};
    int dragging_ = 0, dragOffset_ = 0;
    bool terminalVisible_ = true;
    bool updatingGitFiles_ = false;
    Sidebar activeSidebar_ = Sidebar::Explorer;
    Viewer viewer_;
    Terminal terminal_;
    std::unique_ptr<Worker> worker_, gitWorker_;
    fs::path root_;
    std::vector<Tab> tabs_;
    size_t activeTab_ = 0, firstTab_ = 0, tabTools_ = 0;
    HTREEITEM rootItem_ = nullptr;
    uint64_t generation_ = 0, serial_ = 0, nextId_ = 1;
    std::map<uint64_t, HTREEITEM> nodes_;
    std::shared_ptr<GitResult> git_;
    std::wstring status_ = L"Ready  ·  Ctrl+O: open folder";
    Handle watchDirectory_, watchEvent_;
    OVERLAPPED watchOperation_{};
    alignas(DWORD) std::array<BYTE, 64 * 1024> watchBuffer_{};
    bool watchPending_ = false, dirty_ = false, fileDirty_ = false;
    ULONGLONG dirtySince_ = 0, lastEvent_ = 0;
};
int App::run(HINSTANCE instance, int show, fs::path initial) {
    initializeRendering();
    instance_ = instance;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_TREEVIEW_CLASSES | ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyMain"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION); RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Poppy", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820, nullptr, nullptr, instance, this);
    if (!window_) return 1;
    ShowWindow(window_, show); UpdateWindow(window_);
    if (!initial.empty()) openFolder(std::move(initial));
    MSG msg{};
    for (;;) {
      HANDLE event = watchPending_ ? watchEvent_.value : nullptr;
      const DWORD wait = MsgWaitForMultipleObjects(event ? 1 : 0, event ? &event : nullptr, FALSE, INFINITE, QS_ALLINPUT);
      if (event && wait == WAIT_OBJECT_0) pollWatch();
      if (wait == WAIT_FAILED) return 1;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return static_cast<int>(msg.wParam);
        const bool key = msg.message == WM_KEYDOWN;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (key && ctrl && shift && msg.wParam == 'E') { selectSidebar(Sidebar::Explorer); continue; }
        if (key && ctrl && shift && msg.wParam == 'G') { selectSidebar(Sidebar::Git); continue; }
        if (key && ctrl && msg.wParam == 'O') { chooseFolder(); continue; }
        if (key && ctrl && msg.wParam == VK_OEM_3) { toggleTerminal(); continue; }
        if (key && ctrl && msg.wParam == VK_TAB && !tabs_.empty()) { selectTab((activeTab_ + (shift ? tabs_.size() - 1 : 1)) % tabs_.size()); SetFocus(viewer_.window()); continue; }
        if (key && ctrl && msg.wParam == 'W' && GetFocus() != terminal_.window()) { closeTab(activeTab_); continue; }
        if (key && msg.wParam == VK_F5 && GetFocus() != terminal_.window()) { refresh(); continue; }
        if (key && msg.wParam == VK_F6 && GetFocus() != terminal_.window()) { showDiff(); continue; }
        if (key && msg.wParam == VK_TAB && GetFocus() != terminal_.window()) {
            const HWND files = activeSidebar_ == Sidebar::Explorer ? (IsWindowVisible(tree_) ? tree_ : explorerButton_) : (IsWindowVisible(gitTree_) ? gitTree_ : gitButton_);
            SetFocus(GetFocus() == files ? viewer_.window() : terminalVisible_ ? terminal_.window() : files); continue;
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
      }
    }
}
void App::create() {
    worker_ = std::make_unique<Worker>(window_);
    gitWorker_ = std::make_unique<Worker>(window_);
    tree_ = CreateWindowExW(0, WC_TREEVIEWW, L"Files", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(1), instance_, nullptr);
    TreeView_SetBkColor(tree_, panel); TreeView_SetTextColor(tree_, foreground); TreeView_SetLineColor(tree_, border);
    gitTree_ = CreateWindowExW(0, WC_TREEVIEWW, L"Git changes", WS_CHILD | WS_TABSTOP | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP | TVS_FULLROWSELECT, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(4), instance_, nullptr);
    TreeView_SetBkColor(gitTree_, panel); TreeView_SetTextColor(gitTree_, foreground);
    explorerButton_ = CreateWindowExW(0, L"BUTTON", L"Explorer", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(explorerCommand), instance_, nullptr);
    gitButton_ = CreateWindowExW(0, L"BUTTON", L"Source Control", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(gitCommand), instance_, nullptr);
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    for (const auto button : {explorerButton_, gitButton_}) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tip.hwnd = window_; tip.uId = reinterpret_cast<UINT_PTR>(button);
        tip.lpszText = const_cast<wchar_t*>(button == explorerButton_ ? L"Explorer (Ctrl+Shift+E)" : L"Source Control (Ctrl+Shift+G)");
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    viewer_.create(window_, instance_); terminal_.create(window_, instance_);
    const BOOL dark = TRUE; DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    dpi(GetDpiForWindow(window_));
}
void App::dpi(UINT value) {
    dpi_ = value; renderer_.setDpi(dpi_);
    if (uiFont_) DeleteObject(uiFont_);
    uiFont_ = makeControlFont(MulDiv(13, dpi_, 96));
    SendMessageW(tree_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    SendMessageW(gitTree_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    TreeView_SetItemHeight(tree_, MulDiv(26, dpi_, 96));
    TreeView_SetItemHeight(gitTree_, MulDiv(28, dpi_, 96));
    toolbar_ = MulDiv(44, dpi_, 96); footer_ = MulDiv(26, dpi_, 96); splitter_ = MulDiv(5, dpi_, 96);
    activityWidth_ = MulDiv(48, dpi_, 96); sidebarHeader_ = MulDiv(34, dpi_, 96); gitHeader_ = MulDiv(56, dpi_, 96);
    viewer_.setDpi(dpi_); terminal_.setDpi(dpi_); layout();
}
void App::layout(bool revealTab) {
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
    MoveWindow(explorerButton_, 0, toolbar_, activityWidth_ - 1, activityWidth_, TRUE);
    MoveWindow(gitButton_, 0, toolbar_ + activityWidth_, activityWidth_ - 1, activityWidth_, TRUE);
    ShowWindow(tree_, activeSidebar_ == Sidebar::Explorer && !root_.empty() ? SW_SHOWNA : SW_HIDE);
    MoveWindow(tree_, activityWidth_, toolbar_ + sidebarHeader_, side, std::max(0, content - sidebarHeader_), TRUE);
    const int gitTop = toolbar_ + sidebarHeader_ + gitHeader_;
    const bool showGitFiles = activeSidebar_ == Sidebar::Git && git_ && git_->error.empty() && !git_->root.empty() && !git_->entries.empty();
    ShowWindow(gitTree_, showGitFiles ? SW_SHOWNA : SW_HIDE);
    MoveWindow(gitTree_, activityWidth_, gitTop, side, std::max(0, toolbar_ + content - gitTop), TRUE);
    const int tabHeight = tabs_.empty() ? 0 : MulDiv(34, dpi_, 96);
    tabsRect_ = {right, toolbar_, width, toolbar_ + tabHeight};
    updateTabs(revealTab);
    MoveWindow(viewer_.window(), right, toolbar_ + tabHeight, std::max(0, width - right), std::max(0, viewerSize - tabHeight), TRUE);
    ShowWindow(terminal_.window(), terminalVisible_ ? SW_SHOWNA : SW_HIDE);
    if (terminalVisible_) MoveWindow(terminal_.window(), right, toolbar_ + viewerSize + splitter_, std::max(0, width - right), terminalSize, TRUE);
    InvalidateRect(window_, nullptr, FALSE);
}
void App::paint() {
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
    dc.label({scaled(480), 0, r.right - scaled(16), toolbar_}, root_.empty() ? L"" : root_.native(), muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
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
        dc.label({left, top + scaled(28), edge, top + gitHeader_}, summary, muted);
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
        for (size_t i = firstTab_; i < std::min(tabs_.size(), firstTab_ + tabCapacity()); ++i) {
            const auto rect = tabBounds(i);
            const bool active = i == activeTab_;
            dc.fill(rect, active ? panel : background);
            if (active) dc.fill({rect.left, rect.top, rect.right, rect.top + scaled(2)}, accent);
            const auto title = tabs_[i].path.filename().native() + (tabs_[i].diff ? L" (diff)" : L"");
            dc.label({rect.left + scaled(12), rect.top, rect.right - scaled(28), rect.bottom}, title, active ? foreground : muted);
            dc.label({rect.right - scaled(28), rect.top, rect.right - scaled(4), rect.bottom}, L"×", muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            dc.fill({rect.right - 1, rect.top, rect.right, rect.bottom}, border);
        }
        if (tabs_.size() > tabCapacity()) {
            const int edge = tabsRect_.right, button = scaled(28);
            dc.label({edge - 2 * button, tabsRect_.top, edge - button, tabsRect_.bottom}, L"‹", firstTab_ ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            dc.label({edge - button, tabsRect_.top, edge, tabsRect_.bottom}, L"›", firstTab_ + tabCapacity() < tabs_.size() ? foreground : muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        dc.fill({tabsRect_.left, tabsRect_.bottom - 1, tabsRect_.right, tabsRect_.bottom}, border);
    }
}
void App::drawActivity(const DRAWITEMSTRUCT& item) {
    const bool git = item.CtlID == gitCommand;
    const bool selected = activeSidebar_ == (git ? Sidebar::Git : Sidebar::Explorer);
    auto& dc = git ? gitRenderer_ : explorerRenderer_;
    if (!dc.begin(item.hDC, item.rcItem, item.hwndItem)) return;
    dc.fill(item.rcItem, selected || (item.itemState & ODS_SELECTED) ? panel : background);
    activityIcon(dc, item.rcItem, git, selected ? foreground : muted, dpi_);
    if (selected) dc.fill({item.rcItem.left, item.rcItem.top + MulDiv(8, dpi_, 96), item.rcItem.left + MulDiv(3, dpi_, 96), item.rcItem.bottom - MulDiv(8, dpi_, 96)}, accent);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) { RECT focus = item.rcItem; InflateRect(&focus, -4, -4); dc.outline(focus, muted); }
    dc.end();
}
void App::selectSidebar(Sidebar sidebar) {
    activeSidebar_ = sidebar; layout();
    InvalidateRect(explorerButton_, nullptr, FALSE); InvalidateRect(gitButton_, nullptr, FALSE);
    SetFocus(sidebar == Sidebar::Explorer ? (IsWindowVisible(tree_) ? tree_ : explorerButton_) : (IsWindowVisible(gitTree_) ? gitTree_ : gitButton_));
}
void App::updateGitFiles() {
    updatingGitFiles_ = true;
    const bool visible = IsWindowVisible(gitTree_) != FALSE;
    if (visible) SendMessageW(gitTree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(gitTree_);
    HTREEITEM selection = nullptr;
    if (git_ && git_->error.empty()) {
        for (size_t i = 0; i < git_->entries.size(); ++i) {
            const auto& entry = git_->entries[i];
            std::wstring text = std::wstring(1, entry.state) + L"  " + entry.path.lexically_relative(root_).native();
            TVINSERTSTRUCTW value{}; value.hParent = TVI_ROOT; value.hInsertAfter = TVI_LAST; value.item.mask = TVIF_TEXT | TVIF_PARAM;
            value.item.pszText = text.data(); value.item.lParam = static_cast<LPARAM>(i);
            const auto item = TreeView_InsertItem(gitTree_, &value);
            if (pathKey(entry.path) == pathKey(selectedPath())) selection = item;
        }
    }
    TreeView_SelectItem(gitTree_, selection);
    if (visible) SendMessageW(gitTree_, WM_SETREDRAW, TRUE, 0);
    updatingGitFiles_ = false;
    layout(false); InvalidateRect(gitTree_, nullptr, FALSE);
}
void App::openGitFile(HTREEITEM item) {
    if (updatingGitFiles_ || !git_ || !item) return;
    TVITEMW value{}; value.mask = TVIF_PARAM; value.hItem = item;
    if (!TreeView_GetItem(gitTree_, &value) || static_cast<size_t>(value.lParam) >= git_->entries.size()) return;
    const auto entry = git_->entries[static_cast<size_t>(value.lParam)];
    openFile(entry.path, entry.state != L'?');
}
void App::status(std::wstring message) { status_ = std::move(message); RECT r{}; GetClientRect(window_, &r); r.top = r.bottom - footer_; InvalidateRect(window_, &r, FALSE); }
void App::chooseFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST); dialog->SetTitle(L"Open workspace folder");
    if (SUCCEEDED(dialog->Show(window_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { openFolder(fs::path(path)); CoTaskMemFree(path); } item->Release();
        }
    }
    dialog->Release();
}
Node* App::node(HTREEITEM item) const { if (!item) return nullptr; TVITEMW value{}; value.mask = TVIF_PARAM; value.hItem = item; return TreeView_GetItem(tree_, &value) ? reinterpret_cast<Node*>(value.lParam) : nullptr; }
void App::clearChildren(HTREEITEM parent) { while (const auto child = TreeView_GetChild(tree_, parent)) TreeView_DeleteItem(tree_, child); }
wchar_t App::stateFor(const fs::path& path) const {
    if (!git_) return 0;
    const auto direct = git_->states.find(pathKey(path));
    return direct != git_->states.end() ? direct->second : 0;
}
void App::updateLabel(HTREEITEM item) {
    const auto n = node(item); if (!n) return;
    std::wstring text = n->path.filename().empty() ? n->path.native() : n->path.filename().native();
    if (const wchar_t state = stateFor(n->path)) text = std::wstring(1, state) + L"  " + text;
    if (n->directory) text += n->reparse ? L"  [link]" : L"\\";
    TVITEMW value{}; value.mask = TVIF_TEXT; value.hItem = item; value.pszText = text.data(); TreeView_SetItem(tree_, &value);
}
HTREEITEM App::insert(HTREEITEM parent, const Entry& entry) {
    if (nodes_.size() >= maxTreeNodes) return nullptr;
    auto n = std::make_unique<Node>(); n->path = entry.path; n->id = nextId_++; n->directory = entry.directory; n->reparse = entry.reparse;
    TVINSERTSTRUCTW value{}; value.hParent = parent; value.hInsertAfter = TVI_LAST; value.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN; value.item.pszText = const_cast<wchar_t*>(L""); value.item.lParam = reinterpret_cast<LPARAM>(n.get()); value.item.cChildren = n->directory && !n->reparse ? 1 : 0;
    const auto item = TreeView_InsertItem(tree_, &value);
    if (item) { nodes_[n->id] = item; n.release(); updateLabel(item); }
    return item;
}
void App::openFolder(fs::path path) {
    std::error_code error; path = fs::absolute(path, error).lexically_normal();
    if (error || !fs::is_directory(path, error)) { status(L"Cannot open folder: " + path.native()); return; }
    stopWatch(); ++generation_; ++serial_; worker_->cancelPending(); gitWorker_->cancelPending(); tabs_.clear(); activeTab_ = firstTab_ = 0; git_.reset();
    root_ = std::move(path); TreeView_DeleteAllItems(tree_); nodes_.clear(); updateGitFiles();
    rootItem_ = insert(TVI_ROOT, {root_, true, false});
    requestDirectory(rootItem_);
    viewer_.message(L"Select a file on the left.\n\nFiles are read only. Your AI CLI can edit them from the terminal.");
    terminal_.setDirectory(root_); watch(); refreshGit();
    SetWindowTextW(window_, (L"Poppy — " + root_.native()).c_str()); status(L"F5: refresh  ·  F6: diff  ·  Ctrl+`: terminal"); InvalidateRect(window_, nullptr, FALSE);
}
void App::requestDirectory(HTREEITEM item) {
    const auto n = node(item); if (!n || !n->directory || n->reparse) return;
    n->expandRequested = true;
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
            // A tree item without children cannot expand. Populate it before
            // expanding instead of discarding the first asynchronous result.
            if (n->loaded) TreeView_Expand(tree_, found->second, TVE_EXPAND);
        });
    });
}
void App::applyDirectory(HTREEITEM item, Node*, const DirectoryResult& result) {
    if (!result.error.empty()) { status(L"Folder: " + result.error); return; }
    std::map<std::wstring, HTREEITEM> existing;
    for (auto child = TreeView_GetChild(tree_, item); child; child = TreeView_GetNextSibling(tree_, child)) if (const auto n = node(child)) existing[pathKey(n->path)] = child;
    const bool visible = IsWindowVisible(tree_) != FALSE;
    if (visible) SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    for (const auto& entry : result.entries) {
        const auto found = existing.find(pathKey(entry.path));
        if (found != existing.end()) {
            auto n = node(found->second);
            if (n && n->directory == entry.directory && n->reparse == entry.reparse) { existing.erase(found); continue; }
            TreeView_DeleteItem(tree_, found->second); existing.erase(found);
        }
        insert(item, entry);
    }
    for (const auto& stale : existing) TreeView_DeleteItem(tree_, stale.second);
    TVSORTCB sort{}; sort.hParent = item;
    sort.lpfnCompare = [](LPARAM a, LPARAM b, LPARAM) -> int {
        const auto x = reinterpret_cast<Node*>(a), y = reinterpret_cast<Node*>(b);
        if (x->directory != y->directory) return x->directory ? -1 : 1;
        return CompareStringOrdinal(x->path.filename().c_str(), -1, y->path.filename().c_str(), -1, TRUE) - CSTR_EQUAL;
    };
    TreeView_SortChildrenCB(tree_, &sort, FALSE);
    if (visible) SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree_, nullptr, FALSE);
    if (result.truncated || nodes_.size() >= maxTreeNodes) status(L"Explorer limit: 10,000 items/folder, 50,000 visible nodes.");
}
size_t App::tabCapacity() const {
    const int width = std::max(0L, tabsRect_.right - tabsRect_.left), tabWidth = MulDiv(180, dpi_, 96);
    const int arrows = tabs_.size() > static_cast<size_t>(width / tabWidth) ? MulDiv(56, dpi_, 96) : 0;
    return static_cast<size_t>(std::max(1, (width - arrows) / tabWidth));
}
RECT App::tabBounds(size_t index) const {
    const int left = tabsRect_.left + static_cast<int>(index - firstTab_) * MulDiv(180, dpi_, 96);
    return {left, tabsRect_.top, left + MulDiv(180, dpi_, 96), tabsRect_.bottom};
}
size_t App::tabAt(POINT point) const {
    if (PtInRect(&tabsRect_, point)) {
        for (size_t i = firstTab_; i < std::min(tabs_.size(), firstTab_ + tabCapacity()); ++i) {
            const auto rect = tabBounds(i);
            if (PtInRect(&rect, point)) return i;
        }
    }
    return tabs_.size();
}
void App::updateTabs(bool reveal) {
    const size_t capacity = tabCapacity();
    firstTab_ = std::min(firstTab_, tabs_.size() > capacity ? tabs_.size() - capacity : 0);
    if (reveal && !tabs_.empty()) {
        if (activeTab_ < firstTab_) firstTab_ = activeTab_;
        else if (activeTab_ >= firstTab_ + capacity) firstTab_ = activeTab_ - capacity + 1;
    }
    // Only visible tabs need tooltip regions. Resolve their paths on demand.
    for (size_t i = 0; i < tabTools_; ++i) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.hwnd = window_; tip.uId = 100 + i;
        SendMessageW(tooltips_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    tabTools_ = 0;
    for (size_t i = firstTab_; i < std::min(tabs_.size(), firstTab_ + capacity); ++i) {
        TOOLINFOW tip{}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_SUBCLASS; tip.hwnd = window_; tip.uId = 100 + tabTools_++;
        tip.rect = tabBounds(i); tip.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    }
    InvalidateRect(window_, &tabsRect_, FALSE);
}
void App::saveTab() {
    if (!tabs_.empty() && viewer_.hasDocument()) {
        auto& tab = tabs_[activeTab_];
        (tab.diff ? tab.diffView : tab.fileView) = viewer_.state();
    }
}
void App::openFile(const fs::path& path, bool diff) {
    const auto key = pathKey(path);
    const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return pathKey(tab.path) == key; });
    const size_t index = static_cast<size_t>(found - tabs_.begin());
    if (found != tabs_.end() && index == activeTab_ && (!diff || found->diff)) { updateTabs(); return; }
    saveTab();
    if (found == tabs_.end()) { Tab tab; tab.path = path; tab.diff = diff; tabs_.push_back(std::move(tab)); }
    else if (diff) found->diff = true;
    activeTab_ = index; layout(); loadTab();
}
void App::selectTab(size_t index) {
    if (index >= tabs_.size() || index == activeTab_) return;
    saveTab(); activeTab_ = index; updateTabs(); loadTab();
}
void App::closeTab(size_t index) {
    if (index >= tabs_.size()) return;
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
void App::loadTab(bool preserve) {
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
    if (!git_ || git_->root.empty() || !git_->error.empty()) { viewer_.message(L"Git repository unavailable. Refresh after installing Git for Windows.", path); return; }
    const auto root = git_->root;
    gitWorker_->submit(L"diff", [this, serial, generation, path, root, preserve, state] {
        const auto relative = path.lexically_relative(root).native();
        auto result = runProcess({L"git.exe", L"--no-optional-locks", L"diff", L"--no-ext-diff", L"--no-textconv", L"--color=never", L"--", relative}, root, 5000, gitWorker_->cancellation());
        bool staged = false;
        if (result.code == 0 && result.output.empty()) { staged = true; result = runProcess({L"git.exe", L"--no-optional-locks", L"diff", L"--cached", L"--no-ext-diff", L"--no-textconv", L"--color=never", L"--", relative}, root, 5000, gitWorker_->cancellation()); }
        auto doc = std::make_shared<Document>();
        if (result.code == 0 && static_cast<size_t>(std::count(result.output.begin(), result.output.end(), '\n')) >= maxLines) { result.code = 1; result.error = L"Diff exceeds the 1,000,000-line preview limit."; }
        if (result.code == 0) { doc->text = std::move(result.output); doc->language = Language::Diff; indexDocument(*doc); }
        const auto error = result.error; const auto code = result.code;
        gitWorker_->complete([this, serial, generation, path, doc, code, error, staged, preserve, state] {
            if (generation != generation_ || serial != serial_) return;
            if (code) viewer_.message(error, path);
            else if (doc->text.empty()) viewer_.message(L"No tracked changes for this file.\nUntracked files can be read in the normal preview.\n\nPress F6 to return to the file.", path);
            else { viewer_.show(doc, path, preserve); if (!preserve) viewer_.restore(state); }
            status(staged ? L"Staged diff  ·  F6: return to file" : L"Working tree diff  ·  F6: return to file");
        });
    });
}
void App::showDiff() {
    if (tabs_.empty()) { status(L"Select a file to view its Git diff."); return; }
    auto& tab = tabs_[activeTab_];
    if (!tab.diff && (!git_ || git_->root.empty() || !git_->error.empty())) { status(L"Git repository unavailable. Refresh after installing Git for Windows."); return; }
    saveTab(); tab.diff = !tab.diff; updateTabs(); loadTab();
}
void App::refreshGit() {
    if (root_.empty()) return;
    const auto generation = generation_; const auto root = root_;
    gitWorker_->submit(L"git", [this, root, generation] {
        auto result = readGit(root, gitWorker_->cancellation());
        gitWorker_->complete([this, generation, result] {
            if (generation != generation_) return;
            git_ = result;
            for (const auto& pair : nodes_) updateLabel(pair.second);
            updateGitFiles();
            if (!result->error.empty()) status(L"Git: " + result->error);
            else if (result->truncated) status(L"Git display limited to 50,000 files and badge paths.");
            InvalidateRect(window_, nullptr, FALSE);
        });
    });
}
void App::refresh(bool force) {
    if (root_.empty()) return;
    for (const auto& pair : nodes_) { const auto n = node(pair.second); if (n && n->expandRequested) requestDirectory(pair.second); }
    refreshGit();
    if (!tabs_.empty() && (force || fileDirty_)) { saveTab(); loadTab(true); }
    fileDirty_ = false;
}
void App::watch() {
    watchDirectory_.reset(CreateFileW(root_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
    watchEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!watchDirectory_ || !watchEvent_) { status(L"Folder watching unavailable; use F5 to refresh."); stopWatch(); return; }
    watchOperation_ = {}; watchOperation_.hEvent = watchEvent_.value; armWatch();
}
void App::armWatch() {
    if (!watchDirectory_) return;
    ResetEvent(watchEvent_.value);
    watchPending_ = ReadDirectoryChangesW(watchDirectory_.value, watchBuffer_.data(), static_cast<DWORD>(watchBuffer_.size()), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, nullptr, &watchOperation_, nullptr) != FALSE;
    if (!watchPending_) status(L"Folder watching unavailable; use F5 to refresh.");
}
void App::pollWatch() {
    const auto now = GetTickCount64();
    if (watchPending_ && WaitForSingleObject(watchEvent_.value, 0) == WAIT_OBJECT_0) {
        DWORD bytes = 0; const bool ok = GetOverlappedResult(watchDirectory_.value, &watchOperation_, &bytes, FALSE) != FALSE;
        bool relevant = !ok || !bytes; if (!bytes) fileDirty_ = true;
        for (size_t offset = 0; ok && offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytes;) {
            const auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(watchBuffer_.data() + offset);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const bool gitNoise = name.rfind(L".git\\", 0) == 0 && name != L".git\\HEAD" && name != L".git\\index" && name.rfind(L".git\\refs\\", 0) != 0;
            if (!gitNoise) relevant = true;
            if (!tabs_.empty()) {
                const auto changed = pathKey(root_ / name), selected = pathKey(selectedPath());
                if (changed == selected || selected.rfind(changed + L"\\", 0) == 0) fileDirty_ = true;
            }
            if (!info->NextEntryOffset) break;
            offset += info->NextEntryOffset;
        }
        if (relevant) { if (!dirty_) { dirtySince_ = now; SetTimer(window_, timerId, 200, nullptr); } dirty_ = true; lastEvent_ = now; }
        watchPending_ = false; armWatch();
    }
    if (dirty_ && (now - lastEvent_ >= 500 || now - dirtySince_ >= 2000)) { dirty_ = false; KillTimer(window_, timerId); refresh(false); }
}
void App::stopWatch() {
    KillTimer(window_, timerId);
    if (watchDirectory_ && watchPending_) { CancelIoEx(watchDirectory_.value, &watchOperation_); DWORD bytes = 0; GetOverlappedResult(watchDirectory_.value, &watchOperation_, &bytes, TRUE); }
    watchDirectory_.reset(); watchEvent_.reset(); watchPending_ = dirty_ = fileDirty_ = false;
}
void App::toggleTerminal() {
    terminalVisible_ = !terminalVisible_; layout(); SetFocus(terminalVisible_ ? terminal_.window() : viewer_.window());
}
LRESULT CALLBACK App::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT App::handle(UINT m, WPARAM a, LPARAM b) {
    switch (m) {
    case WM_CREATE: create(); return 0;
    case WM_PAINT: paint(); return 0;
    case WM_DRAWITEM:
        if (a == explorerCommand || a == gitCommand) { drawActivity(*reinterpret_cast<DRAWITEMSTRUCT*>(b)); return TRUE; }
        break;
    case WM_COMMAND:
        if (HIWORD(a) == BN_CLICKED) {
            if (LOWORD(a) == explorerCommand) { selectSidebar(Sidebar::Explorer); return 0; }
            if (LOWORD(a) == gitCommand) { selectSidebar(Sidebar::Git); return 0; }
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: { auto info = reinterpret_cast<MINMAXINFO*>(b); info->ptMinTrackSize = {MulDiv(720, dpi_, 96), MulDiv(480, dpi_, 96)}; return 0; }
    case WM_DPICHANGED: { dpi(HIWORD(a)); const auto r = reinterpret_cast<RECT*>(b); SetWindowPos(window_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case wmWork: worker_->dispatch(); gitWorker_->dispatch(); return 0;
    case WM_TIMER: if (a == timerId) pollWatch(); return 0;
    case WM_ACTIVATE: if (LOWORD(a) != WA_INACTIVE && worker_ && !root_.empty()) refresh(false); return 0;
    case WM_LBUTTONDOWN: {
        const int x = MulDiv(GET_X_LPARAM(b), 96, dpi_), y = GET_Y_LPARAM(b);
        if (y < toolbar_) {
            if (x >= 98 && x < 210) chooseFolder(); else if (x >= 220 && x < 292) refresh(); else if (x >= 305 && x < 360) showDiff(); else if (x >= 374 && x < 460) toggleTerminal();
            return 0;
        }
        const POINT point{GET_X_LPARAM(b), y};
        if (PtInRect(&tabsRect_, point)) {
            const auto index = tabAt(point);
            if (index < tabs_.size()) {
                const auto rect = tabBounds(index);
                if (point.x >= rect.right - MulDiv(28, dpi_, 96)) closeTab(index);
                else { selectTab(index); SetFocus(viewer_.window()); }
            } else if (tabs_.size() > tabCapacity()) {
                const int button = MulDiv(28, dpi_, 96);
                if (point.x >= tabsRect_.right - button && firstTab_ + tabCapacity() < tabs_.size()) ++firstTab_;
                else if (point.x >= tabsRect_.right - 2 * button && point.x < tabsRect_.right - button && firstTab_) --firstTab_;
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
    case WM_MOUSEMOVE:
        if (dragging_) {
            RECT r{}; GetClientRect(window_, &r);
            if (dragging_ == 1) sidebar_ = std::clamp(MulDiv(GET_X_LPARAM(b) - activityWidth_ - dragOffset_, 96, dpi_), 130, std::max(130, MulDiv(r.right - activityWidth_ - splitter_, 96, dpi_) - 250));
            else terminalHeight_ = std::clamp(MulDiv(r.bottom - footer_ - GET_Y_LPARAM(b), 96, dpi_), 100, std::max(100, MulDiv(r.bottom - toolbar_ - footer_, 96, dpi_) - 120));
            layout();
        } return 0;
    case WM_LBUTTONUP: dragging_ = 0; if (GetCapture() == window_) ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: dragging_ = 0; return 0;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(a) == window_ && LOWORD(b) == HTCLIENT) {
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
        if (header->hwndFrom == gitTree_) {
            if (header->code == TVN_SELCHANGEDW) openGitFile(reinterpret_cast<NMTREEVIEWW*>(b)->itemNew.hItem);
            else if (header->code == NM_RETURN) openGitFile(TreeView_GetSelection(gitTree_));
            else if (header->code == NM_CLICK) {
                const DWORD position = GetMessagePos(); TVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(position), GET_Y_LPARAM(position)};
                ScreenToClient(gitTree_, &hit.pt); TreeView_HitTest(gitTree_, &hit);
                if (hit.hItem == TreeView_GetSelection(gitTree_)) openGitFile(hit.hItem);
            }
            else if (header->code == NM_CUSTOMDRAW) {
                const auto draw = reinterpret_cast<NMTVCUSTOMDRAW*>(b);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && !(draw->nmcd.uItemState & CDIS_SELECTED) && git_ && static_cast<size_t>(draw->nmcd.lItemlParam) < git_->entries.size()) draw->clrText = stateColor(git_->entries[static_cast<size_t>(draw->nmcd.lItemlParam)].state);
            }
            return 0;
        }
        if (header->hwndFrom != tree_) break;
        if (header->code == NM_CLICK || header->code == NM_RETURN) {
            auto item = TreeView_GetSelection(tree_);
            if (header->code == NM_CLICK) {
                const DWORD position = GetMessagePos(); TVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(position), GET_Y_LPARAM(position)};
                ScreenToClient(tree_, &hit.pt); TreeView_HitTest(tree_, &hit);
                if (!(hit.flags & TVHT_ONITEM) || hit.hItem != item) return 0;
            }
            if (const auto n = node(item); n && !n->directory) openFile(n->path);
        } else if (header->code == TVN_ITEMEXPANDINGW) {
            const auto change = reinterpret_cast<NMTREEVIEWW*>(b);
            if (change->action == TVE_EXPAND) {
                auto n = node(change->itemNew.hItem);
                if (n && !n->loaded) { requestDirectory(change->itemNew.hItem); return TRUE; }
            }
        } else if (header->code == TVN_ITEMEXPANDEDW) {
            const auto change = reinterpret_cast<NMTREEVIEWW*>(b);
            if (change->action == TVE_COLLAPSE) {
                auto n = node(change->itemNew.hItem);
                if (n) {
                    n->expandRequested = false; clearChildren(change->itemNew.hItem); n->loaded = false;
                    TVITEMW value{}; value.mask = TVIF_CHILDREN; value.hItem = change->itemNew.hItem; value.cChildren = n->directory && !n->reparse ? 1 : 0; TreeView_SetItem(tree_, &value);
                }
            }
        } else if (header->code == TVN_SELCHANGEDW) {
            const auto change = reinterpret_cast<NMTREEVIEWW*>(b); if (const auto n = node(change->itemNew.hItem); n && !n->directory) openFile(n->path);
        } else if (header->code == TVN_DELETEITEMW) {
            const auto change = reinterpret_cast<NMTREEVIEWW*>(b); const auto n = reinterpret_cast<Node*>(change->itemOld.lParam); if (n) { nodes_.erase(n->id); delete n; }
        } else if (header->code == NM_CUSTOMDRAW) {
            const auto draw = reinterpret_cast<NMTVCUSTOMDRAW*>(b);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && !(draw->nmcd.uItemState & CDIS_SELECTED)) {
                const auto n = reinterpret_cast<Node*>(draw->nmcd.lItemlParam); const auto state = n ? stateFor(n->path) : 0;
                if (state) draw->clrText = stateColor(state);
            }
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(window_); return 0;
    case WM_DESTROY:
        KillTimer(window_, timerId); stopWatch(); if (gitWorker_) gitWorker_->stop(); if (worker_) worker_->stop(); terminal_.stop(); TreeView_DeleteAllItems(tree_);
        if (uiFont_) { DeleteObject(uiFont_); uiFont_ = nullptr; } PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(window_, m, a, b);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    poppy::fs::path initial; if (argv && argc > 1) initial = argv[1]; if (argv) LocalFree(argv);
    int result = 1;
    try { poppy::App app; result = app.run(instance, show, std::move(initial)); }
    catch (const std::exception& error) { MessageBoxW(nullptr, poppy::wide(error.what()).c_str(), L"Poppy", MB_OK | MB_ICONERROR); }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
