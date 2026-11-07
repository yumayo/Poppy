#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <set>
#include <vector>
#include "tree_messages.h"
#include "activity_button.h"
#include "layout_config.h"
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace poppy;
namespace {
constexpr int projectRowHeight = 118; // Project heading plus three message lines, padding and card spacing.
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate>
void waitFor(Predicate predicate, const char* message) {
    const auto start = GetTickCount64();
    do {
        if (predicate()) return;
        Sleep(10);
    } while (GetTickCount64() - start < 5000);
    throw std::runtime_error(message);
}
LRESULT send(HWND window, UINT message, WPARAM a = 0, LPARAM b = 0) {
    DWORD_PTR result = 0;
    check(SendMessageTimeoutW(window, message, a, b, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0,
          "Poppy did not respond to a tree message");
    return static_cast<LRESULT>(result);
}
TreeItem nextItem(HWND tree, TreeRelation relation, TreeItem item = 0) {
    return static_cast<TreeItem>(send(tree, treeGetNext, static_cast<WPARAM>(relation), static_cast<LPARAM>(item)));
}
// Each repository starts with Compare, followed by the current branch.
TreeItem gitBranch(HWND tree, TreeItem repository = 0) {
    const auto compare = repository ? nextItem(tree, TreeRelation::Child, repository) : nextItem(tree, TreeRelation::Root);
    return nextItem(tree, TreeRelation::Next, compare);
}
TreeItem gitGroup(HWND tree, bool staged, TreeItem repository = 0) {
    const auto branch = gitBranch(tree, repository);
    if (!branch) return 0;
    const auto first = nextItem(tree, TreeRelation::Child, branch);
    return staged ? first : nextItem(tree, TreeRelation::Next, first);
}
size_t subtreeCount(HWND tree, TreeItem item) {
    if (!item) return 0;
    size_t count = 1;
    for (auto child = nextItem(tree, TreeRelation::Child, item); child; child = nextItem(tree, TreeRelation::Next, child)) count += subtreeCount(tree, child);
    return count;
}
size_t gitChangeCount(HWND tree) {
    return subtreeCount(tree, gitGroup(tree, true)) + subtreeCount(tree, gitGroup(tree, false));
}
bool expanded(HWND tree, TreeItem item) {
    return send(tree, treeGetExpanded, static_cast<WPARAM>(item)) != 0;
}
void expand(HWND tree, TreeItem item, bool value = true) {
    // Exercise keyboard navigation and the asynchronous expansion callback.
    send(tree, treeSelect, static_cast<WPARAM>(item));
    send(tree, WM_KEYDOWN, value ? VK_RIGHT : VK_LEFT);
}
struct Fixture {
    inline static size_t serial = 0;
    fs::path root;
    Fixture() : root(fs::temp_directory_path() / (L"Poppy folder test 日本語 " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()) + L" " + std::to_wstring(++serial))) {
        check(fs::create_directory(root), "Could not create the test workspace");
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};
struct AppDataEnvironment {
    std::wstring previous;
    bool existed = false;
    explicit AppDataEnvironment(const fs::path& directory) {
        const DWORD size = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
        if (size) {
            previous.resize(size); const DWORD count = GetEnvironmentVariableW(L"APPDATA", previous.data(), size);
            check(count && count < size, "Cannot read APPDATA for test isolation"); previous.resize(count); existed = true;
        }
        check(SetEnvironmentVariableW(L"APPDATA", directory.c_str()) != FALSE, "Cannot isolate application storage");
    }
    ~AppDataEnvironment() { SetEnvironmentVariableW(L"APPDATA", existed ? previous.c_str() : nullptr); }
};
void writeFile(const fs::path& path) {
    std::ofstream file(path);
    file << "Poppy folder regression test\n";
    file.close();
    check(!file.fail(), "Could not create a test file");
}
std::string readText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    check(file.is_open(), "Could not read a test file");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void writeLines(const fs::path& path, int lines) {
    std::ofstream file(path);
    // Keep tab/session fixtures on one display row. Long-line wrapping and
    // horizontal scrolling with wrapping disabled have dedicated viewer tests.
    for (int i = 0; i < lines; ++i) file << i << " example\n";
    file.close();
    check(!file.fail(), "Could not create a tab test file");
}
struct Application {
    PROCESS_INFORMATION process{};
    HWND window = nullptr, host = nullptr;
    Application(const fs::path& executable, const fs::path& folder = {}) {
        std::wstring command = L"\"" + executable.native() + L"\"";
        if (!folder.empty()) command += L" \"" + folder.native() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE,
              "Could not launch Poppy");
        CloseHandle(process.hThread); process.hThread = nullptr;
    }
    ~Application() {
        if (host) PostMessageW(host, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process.hProcess, 2000) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
        }
        CloseHandle(process.hProcess);
    }
    HWND tree() {
        HWND result = nullptr;
        waitFor([&] {
            check(WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT, "Poppy exited before loading the workspace");
            EnumWindows([](HWND candidate, LPARAM context) -> BOOL {
                auto& app = *reinterpret_cast<Application*>(context);
                DWORD id = 0; GetWindowThreadProcessId(candidate, &id);
                if (id != app.process.dwProcessId) return TRUE;
                wchar_t name[32]{}; GetClassNameW(candidate, name, 32);
                if (std::wstring(name) != L"PoppyMain") return TRUE;
                app.host = candidate; return FALSE;
            }, reinterpret_cast<LPARAM>(this));
            if (host) {
                for (HWND project = nullptr; (project = FindWindowExW(host, project, L"PoppyProject", nullptr));) {
                    if (!IsWindowVisible(project)) continue;
                    window = project; result = FindWindowExW(window, nullptr, treeClassName, L"Files");
                    if (result && nextItem(result, TreeRelation::Root)) break;
                    result = nullptr;
                }
            }
            return result != nullptr;
        }, "Poppy did not create its file tree");
        return result;
    }
};
RECT bounds(HWND parent, HWND child) {
    RECT rect{};
    check(GetWindowRect(child, &rect) != FALSE, "Could not read a pane's bounds");
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}
HWND draggedTab(const Application& app) {
    for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, L"PoppyDraggedTab", nullptr));)
        if (GetWindow(window, GW_OWNER) == app.host && IsWindowVisible(window)) return window;
    return nullptr;
}
template<class Predicate>
void controlShortcut(const Application& app, HWND target, WPARAM key, Predicate completed, const char* message) {
    struct Input {
        DWORD thread; BYTE previous[256]{};
        explicit Input(DWORD target) : thread(target) {
            check(AttachThreadInput(GetCurrentThreadId(), thread, TRUE) != FALSE, "Cannot attach shortcut input");
            GetKeyboardState(previous);
        }
        ~Input() { SetKeyboardState(previous); AttachThreadInput(GetCurrentThreadId(), thread, FALSE); }
    } input(app.process.dwThreadId);
    SetFocus(target);
    BYTE keys[256]{}; keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
    check(SetKeyboardState(keys) && PostMessageW(target, WM_KEYDOWN, key, 0), "Cannot post shortcut");
    waitFor(completed, message);
}
void controlKey(const Application& app, HWND target, WPARAM key, bool shift = false) {
    const bool redo = key == 'Y' || shift;
    waitFor([&] { return send(target, redo ? treeCanRedo : treeCanUndo) != 0; }, "Explorer operation did not become available");
    check(AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, TRUE) != FALSE, "Cannot attach Explorer history input");
    BYTE previous[256]{}, keys[256]{}; GetKeyboardState(previous);
    keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
    if (shift) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
    SetKeyboardState(keys); send(target, WM_KEYDOWN, key); SetKeyboardState(previous);
    AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, FALSE);
    waitFor([&] { return send(target, redo ? treeCanUndo : treeCanRedo) != 0; }, "Explorer Undo/Redo did not finish");
}
SCROLLINFO scrollInfo(HWND window, int bar = SB_VERT) {
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
    check(GetScrollInfo(window, bar, &info) != FALSE, "Could not read the viewer's scroll position");
    return info;
}
int lastTextRow(HWND viewer) {
    const auto info = scrollInfo(viewer);
    // Normal editor ranges include one page minus one row of bottom padding.
    return std::max(0, info.nMax - static_cast<int>(info.nPage) + 1);
}
void waitLines(HWND viewer, int lines, bool fixedFilename = false) {
    waitFor([&] {
        const auto info = scrollInfo(viewer);
        const int pinned = fixedFilename && info.nMax > 0 ? 1 : 0;
        return lastTextRow(viewer) + pinned == lines;
    }, "The selected tab did not load the expected document");
}
void click(HWND window, int x, int y, bool middle = false) {
    send(window, middle ? WM_MBUTTONDOWN : WM_LBUTTONDOWN, middle ? MK_MBUTTON : MK_LBUTTON, MAKELPARAM(x, y));
    send(window, middle ? WM_MBUTTONUP : WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}
void discardItem(const Application& app, HWND changes, TreeItem item, int answer, bool all) {
    if (all) expand(changes, item, false);
    RECT rect{}; GetClientRect(changes, &rect);
    const int x = rect.right - MulDiv(all ? 74 : 46, GetDpiForWindow(app.window), 96);
    const int height = static_cast<int>(send(changes, treeGetRowHeight));
    const int y = (static_cast<int>(send(changes, treeGetRow, item)) - scrollInfo(changes).nPos) * height + height / 2;
    // Post the click because its handler opens a modal confirmation.
    PostMessageW(changes, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    PostMessageW(changes, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    PostMessageW(changes, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    HWND dialog = nullptr;
    waitFor([&] {
        dialog = FindWindowW(L"#32770", all ? L"Poppy — Discard All Changes" : L"Poppy — Discard Changes");
        DWORD pid = 0; if (dialog) GetWindowThreadProcessId(dialog, &pid);
        return dialog && pid == app.process.dwProcessId;
    }, "Discard did not ask for confirmation");
    check(send(dialog, DM_GETDEFID) == MAKELONG(IDCANCEL, DC_HASDEFID), "Discard confirmation must default to Cancel");
    send(dialog, WM_COMMAND, answer);
    waitFor([&] { return !IsWindow(dialog); }, "Discard confirmation did not close");
}
void discardGroup(const Application& app, HWND changes, TreeItem group, int answer) { discardItem(app, changes, group, answer, true); }
HWND tabTooltips(const Application& app) {
    HWND result = nullptr;
    struct Context { HWND owner; HWND& result; } context{app.window, result};
    EnumThreadWindows(app.process.dwThreadId, [](HWND window, LPARAM data) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(data);
        wchar_t name[64]{}; GetClassNameW(window, name, 64);
        if (GetWindow(window, GW_OWNER) != context.owner || std::wstring(name) != TOOLTIPS_CLASSW) return TRUE;
        context.result = window; return FALSE;
    }, reinterpret_cast<LPARAM>(&context));
    check(result != nullptr, "The tab tooltips are missing");
    return result;
}
TOOLINFOW dockTabInfo(const Application& app, int visibleIndex) {
    struct RemoteMemory {
        HANDLE process; void* value;
        ~RemoteMemory() { if (value) VirtualFreeEx(process, value, 0, MEM_RELEASE); }
    } memory{app.process.hProcess, VirtualAllocEx(app.process.hProcess, nullptr, sizeof(TOOLINFOW) + 2048, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)};
    check(memory.value != nullptr, "Could not allocate tooltip query memory");
    TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.hwnd = app.window; tool.uId = 100 + visibleIndex;
    tool.lpszText = reinterpret_cast<wchar_t*>(static_cast<char*>(memory.value) + sizeof(tool));
    check(WriteProcessMemory(memory.process, memory.value, &tool, sizeof(tool), nullptr) != FALSE, "Could not write a tooltip query");
    check(send(tabTooltips(app), TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(memory.value)) != FALSE, "A tab tooltip region is missing");
    check(ReadProcessMemory(memory.process, memory.value, &tool, sizeof(tool), nullptr) != FALSE, "Could not read a tab tooltip region");
    return tool;
}
int dockTabCount(const Application& app) { return static_cast<int>(send(tabTooltips(app), TTM_GETTOOLCOUNT)) - 4; }
int visibleTabCount(const Application& app) {
    int files = 0;
    for (int i = 0; i < dockTabCount(app); ++i) if (!dockTabInfo(app, i).lParam) ++files;
    return files;
}
RECT tabBounds(const Application& app, int visibleIndex) {
    for (int i = 0; i < dockTabCount(app); ++i) {
        const auto info = dockTabInfo(app, i);
        if (!info.lParam && visibleIndex-- == 0) return info.rect;
    }
    throw std::runtime_error("An editor tab tooltip region is missing");
}
RECT editorModeTools(const Application& app, HWND viewer, int modeWidth) {
    const auto body = bounds(app.window, viewer);
    const UINT dpi = GetDpiForWindow(app.window);
    const LONG top = body.top - MulDiv(34, dpi, 96);
    bool firstRow = true;
    for (int i = 0; i < dockTabCount(app); ++i) {
        const auto tab = dockTabInfo(app, i).rect;
        if (tab.left >= body.left && tab.left < body.right && tab.bottom == top) firstRow = false;
    }
    const int width = (std::min)(MulDiv(modeWidth, dpi, 96), (static_cast<int>(body.right - body.left) - MulDiv(58, dpi, 96)) / 2);
    const LONG right = body.right - (firstRow ? MulDiv(58, dpi, 96) : 0);
    return {right - width, top, right, body.top};
}
RECT terminalTabBounds(const Application& app, HWND terminal) {
    for (int i = 0; i < dockTabCount(app); ++i) {
        const auto info = dockTabInfo(app, i);
        if (info.lParam == reinterpret_cast<LPARAM>(terminal)) return info.rect;
    }
    throw std::runtime_error("A terminal tab tooltip region is missing");
}
void clickTerminalTab(const Application& app, HWND terminal) {
    const auto rect = terminalTabBounds(app, terminal);
    click(app.window, rect.left + MulDiv(12, GetDpiForWindow(app.window), 96), (rect.top + rect.bottom) / 2);
}
POINT terminalAddPoint(const Application& app, HWND terminal, bool dropdown = false) {
    const auto body = bounds(app.window, terminal);
    auto header = terminalTabBounds(app, terminal);
    for (int i = 0; i < dockTabCount(app); ++i) {
        const auto info = dockTabInfo(app, i);
        if (info.lParam && info.rect.left >= body.left && info.rect.right <= body.right) header.top = (std::min)(header.top, info.rect.top);
    }
    return {body.right - MulDiv(dropdown ? 12 : 41, GetDpiForWindow(app.window), 96), header.top + MulDiv(17, GetDpiForWindow(app.window), 96)};
}
void addTerminal(const Application& app, HWND terminal) { const auto point = terminalAddPoint(app, terminal); click(app.window, point.x, point.y); }
HWND otherVisibleViewer(HWND surface, HWND except) {
    HWND result = nullptr;
    waitFor([&] {
        for (HWND child = nullptr; (child = FindWindowExW(surface, child, L"PoppyViewer", nullptr));)
            if (child != except && IsWindowVisible(child)) { result = child; return true; }
        return false;
    }, "A split editor did not become visible");
    return result;
}
void clickTab(const Application& app, int visibleIndex, bool middle = false, bool close = false) {
    const auto rect = tabBounds(app, visibleIndex);
    const int inset = MulDiv(close ? 14 : 16, GetDpiForWindow(app.window), 96);
    click(app.window, close ? rect.right - inset : rect.left + inset, (rect.top + rect.bottom) / 2, middle);
}
void keepTab(const Application& app, int visibleIndex) {
    clickTab(app, visibleIndex);
    const auto rect = tabBounds(app, visibleIndex);
    const auto point = MAKELPARAM(rect.left + MulDiv(16, GetDpiForWindow(app.window), 96), (rect.top + rect.bottom) / 2);
    send(app.window, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
    send(app.window, WM_LBUTTONUP, 0, point);
}
void beginTabDrag(const Application& app, int visibleIndex, POINT target) {
    const auto rect = tabBounds(app, visibleIndex);
    const POINT origin{rect.left + MulDiv(12, GetDpiForWindow(app.window), 96), (rect.top + rect.bottom) / 2};
    send(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(origin.x, origin.y));
    send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(target.x, target.y));
}
void dragTab(const Application& app, int visibleIndex, POINT target, UINT cancel = 0) {
    beginTabDrag(app, visibleIndex, target);
    if (cancel) send(app.window, cancel);
    send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(target.x, target.y));
}
void tabDragPreview(const Application& app, int visibleIndex, HWND pane, bool splittable = true) {
    const auto body = bounds(app.window, pane);
    const auto source = tabBounds(app, visibleIndex);
    const POINT origin{source.left + MulDiv(12, GetDpiForWindow(app.window), 96), (source.top + source.bottom) / 2};
    const POINT center{(body.left + body.right) / 2, (body.top + body.bottom) / 2};
    beginTabDrag(app, visibleIndex, center);
    const auto preview = FindWindowExW(app.window, nullptr, L"PoppyDropPreview", nullptr);
    const auto tab = draggedTab(app);
    check(preview && tab && IsWindowVisible(preview) && IsWindowVisible(tab), "A same-pane drag has no destination or dragged tab");
    const auto destination = bounds(app.window, preview);
    check(EqualRect(&destination, &body), "The center drop preview covers the tab strip");
    wchar_t text[128]{}; GetWindowTextW(preview, text, 128);
    check(std::wstring(text) == L"Move tab here", "The unchanged center destination has no guidance");
    GetWindowTextW(tab, text, 128);
    check(text[0] != 0 && send(tab, WM_NCHITTEST) == HTTRANSPARENT, "The dragged tab has no title or intercepts mouse input");
    const auto follows = [&](POINT point) {
        auto expected = source;
        OffsetRect(&expected, point.x - origin.x, point.y - origin.y);
        const auto actual = bounds(app.window, tab);
        check(EqualRect(&actual, &expected), "The dragged tab lost the grabbed point or stopped at the client edge");
    };
    follows(center);
    send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(body.left + 1, body.top + 1));
    const auto firstTab = bounds(app.window, tab);
    send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(body.left + 9, body.top + 8));
    const auto movedTab = bounds(app.window, tab);
    check(movedTab.left - firstTab.left == 8 && movedTab.top - firstTab.top == 7, "The dragged tab does not follow the mouse");
    struct Target { POINT point; RECT preview; };
    const Target targets[] = {
        {{body.left + 1, center.y}, {body.left, body.top, center.x, body.bottom}},
        {{body.right - 1, center.y}, {center.x, body.top, body.right, body.bottom}},
        {{center.x, body.top + 1}, {body.left, body.top, body.right, center.y}},
        {{center.x, body.bottom - 1}, {body.left, center.y, body.right, body.bottom}}
    };
    for (const auto& target : targets) {
        send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(target.point.x, target.point.y));
        follows(target.point);
        const auto rect = bounds(app.window, preview), actualBody = bounds(app.window, pane);
        const auto expected = splittable ? target.preview : body;
        check(IsWindowVisible(preview) && EqualRect(&rect, &expected), "A directional drop preview includes tabs or disappears for a sole tab");
        check(EqualRect(&actualBody, &body), "Previewing a drop changed the pane layout");
    }
    RECT client{}; GetClientRect(app.window, &client);
    const POINT outside[] = {{client.right - 1, client.bottom - 1}, {client.right + 30, client.bottom + 30}, {-20, -20}};
    for (const auto point : outside) {
        send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        follows(point);
    }
    check(!IsWindowVisible(preview) && IsWindowVisible(tab), "Leaving drop targets hid the dragged tab");
    send(app.window, WM_CANCELMODE);
    check(!IsWindowVisible(preview) && !IsWindowVisible(tab), "Cancelling a drag left an overlay visible");
    beginTabDrag(app, visibleIndex, center);
    send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(center.x, center.y));
    const auto after = bounds(app.window, pane);
    check(EqualRect(&after, &body) && !IsWindowVisible(preview) && !IsWindowVisible(tab), "An unchanged center drop changed layout or left an overlay visible");
}
void dragTerminalTab(const Application& app, HWND terminal, POINT target, UINT cancel = 0) {
    const auto rect = terminalTabBounds(app, terminal);
    const POINT origin{rect.left + MulDiv(12, GetDpiForWindow(app.window), 96), (rect.top + rect.bottom) / 2};
    send(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(origin.x, origin.y));
    send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(target.x, target.y));
    auto expected = rect;
    OffsetRect(&expected, target.x - origin.x, target.y - origin.y);
    const auto actual = bounds(app.window, draggedTab(app));
    check(EqualRect(&actual, &expected), "The dragged terminal tab does not follow the grabbed point");
    if (cancel == WM_KEYDOWN) {
        PostMessageW(terminal, WM_KEYDOWN, VK_ESCAPE, 0);
        waitFor([&] { GUITHREADINFO info{}; info.cbSize = sizeof(info); return GetGUIThreadInfo(app.process.dwThreadId, &info) && info.hwndCapture != app.window; }, "Escape from a terminal did not cancel docking");
    } else if (cancel) send(app.window, cancel);
    send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(target.x, target.y));
}
enum class TabMenuCommand { Pin, CopyPath, CopyRelativePath, CloseOthers, CloseAll };
void tabMenuCommand(const Application& app, int visibleIndex, TabMenuCommand command, bool wait = true) {
    const auto rect = tabBounds(app, visibleIndex);
    POINT point{rect.left + 1, (rect.top + rect.bottom) / 2}; ClientToScreen(app.window, &point);
    check(PostMessageW(app.window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(app.window), MAKELPARAM(point.x, point.y)) != FALSE,
          "Could not open a tab's context menu");
    const auto menuOpen = [&] {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        check(GetGUIThreadInfo(app.process.dwThreadId, &info) != FALSE, "Could not read the menu state");
        return (info.flags & GUI_INMENUMODE) != 0;
    };
    waitFor(menuOpen, "The tab context menu did not open");
    // Path commands follow Pin; bulk close commands are the last two entries.
    const bool fromStart = command == TabMenuCommand::Pin || command == TabMenuCommand::CopyPath || command == TabMenuCommand::CopyRelativePath;
    PostMessageW(app.window, WM_KEYDOWN, fromStart ? VK_HOME : VK_END, 0);
    if (command == TabMenuCommand::CopyPath || command == TabMenuCommand::CopyRelativePath) PostMessageW(app.window, WM_KEYDOWN, VK_DOWN, 0);
    if (command == TabMenuCommand::CopyRelativePath) PostMessageW(app.window, WM_KEYDOWN, VK_DOWN, 0);
    if (command == TabMenuCommand::CloseOthers) PostMessageW(app.window, WM_KEYDOWN, VK_UP, 0);
    PostMessageW(app.window, WM_KEYDOWN, VK_RETURN, 0);
    waitFor([&] { return !menuOpen(); }, "The tab context menu did not close");
    // A save confirmation keeps the original context-menu handler running.
    if (wait) send(app.window, WM_NULL);
}
void toggleTabPin(const Application& app, int visibleIndex) {
    tabMenuCommand(app, visibleIndex, TabMenuCommand::Pin);
}
void tabCopyPathTests(const fs::path& executable) {
    Fixture fixture;
    const auto firstPath = fixture.root / L"a 日本語.txt";
    writeLines(firstPath, 200); writeLines(fixture.root / L"b.txt", 300);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 3; }, "Path copy fixture did not load");
    const auto first = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    send(tree, treeSelect, first); waitLines(viewer, 200); keepTab(app, 0);
    send(tree, treeSelect, nextItem(tree, TreeRelation::Next, first)); waitLines(viewer, 300);
    const auto clipboard = [&] {
        check(OpenClipboard(app.host) != FALSE, "Cannot read the path clipboard");
        const auto value = GetClipboardData(CF_UNICODETEXT);
        const auto data = value ? static_cast<const wchar_t*>(GlobalLock(value)) : nullptr;
        const std::wstring result = data ? data : L"";
        if (data) GlobalUnlock(value);
        CloseClipboard(); return result;
    };
    const auto copy = [&](TabMenuCommand command, const std::wstring& expected) {
        tabMenuCommand(app, 0, command);
        check(clipboard() == expected, "A tab path command copied the wrong path");
        check(lastTextRow(viewer) == 300 && visibleTabCount(app) == 2, "Copying an inactive tab changed the active file or tabs");
    };
    copy(TabMenuCommand::CopyPath, firstPath.native());
    copy(TabMenuCommand::CopyRelativePath, L"a 日本語.txt");
    toggleTabPin(app, 0);
    copy(TabMenuCommand::CopyPath, firstPath.native());
    copy(TabMenuCommand::CopyRelativePath, L"a 日本語.txt");
}
void answerTabSave(const Application& app, const std::wstring& filename, int answer) {
    HWND dialog = nullptr;
    waitFor([&] {
        dialog = FindWindowW(L"#32770", L"Poppy");
        DWORD pid = 0; if (dialog) GetWindowThreadProcessId(dialog, &pid);
        return dialog && pid == app.process.dwProcessId;
    }, "Closing tabs did not ask about unsaved edits");
    check(FindWindowExW(dialog, nullptr, L"Static", (L"Save changes to " + filename + L"?").c_str()) != nullptr,
          "Bulk close asked about a protected tab or the wrong file");
    send(dialog, WM_COMMAND, answer);
    waitFor([&] { return !IsWindow(dialog); }, "The tab save confirmation did not close");
}
void navigationHistoryTests(const fs::path& executable) {
    Fixture fixture, other;
    writeLines(fixture.root / L"a.txt", 200); writeLines(fixture.root / L"b.txt", 300);
    writeLines(fixture.root / L"c.txt", 400); writeLines(other.root / L"d.txt", 500);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), surface = app.window;
    const auto left = FindWindowExW(surface, nullptr, L"PoppyViewer", nullptr);
    HWND right = nullptr;
    waitFor([&] { return send(tree, treeGetCount) == 4; }, "Navigation fixture did not load");
    const auto a = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    const auto b = nextItem(tree, TreeRelation::Next, a), c = nextItem(tree, TreeRelation::Next, b);
    const auto focused = [&](HWND expected) {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        return GetGUIThreadInfo(app.process.dwThreadId, &info) && info.hwndFocus == expected;
    };
    const auto mouse = [](HWND target, bool forward) {
        const auto button = MAKEWPARAM(0, forward ? XBUTTON2 : XBUTTON1);
        check(PostMessageW(target, WM_XBUTTONDOWN, button, 0) && PostMessageW(target, WM_XBUTTONUP, button, 0),
            "Cannot post navigation mouse buttons");
    };
    const auto command = [](HWND target, bool forward) {
        const auto code = forward ? APPCOMMAND_BROWSER_FORWARD : APPCOMMAND_BROWSER_BACKWARD;
        check(send(target, WM_APPCOMMAND, reinterpret_cast<WPARAM>(target), MAKELPARAM(0, code | FAPPCOMMAND_MOUSE)) == TRUE,
            "Navigation app command was not handled");
    };
    send(tree, treeSelect, a); waitLines(left, 200);
    send(left, WM_VSCROLL, SB_PAGEDOWN); const auto top = scrollInfo(left).nPos;
    send(tree, treeSelect, b); waitLines(left, 300);
    mouse(left, false); waitLines(left, 200);
    check(visibleTabCount(app) == 1 && scrollInfo(left).nPos == top && focused(left),
        "Back did not restore a replaced preview, its position and focus");
    mouse(left, true); waitLines(left, 300);
    command(tree, false); waitLines(left, 200);
    send(tree, treeSelect, c); waitLines(left, 400); keepTab(app, 0);
    send(app.host, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2)); send(app.host, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2));
    waitLines(left, 400);
    check(visibleTabCount(app) == 1, "A new activation retained Forward or accumulated previews");
    send(tree, treeSelect, a); waitLines(left, 200); keepTab(app, 1);
    send(tree, treeSelect, b); waitLines(left, 300);
    clickTab(app, 1, false, true); // Close inactive a; all visits to it must be skipped.
    command(left, false); waitLines(left, 400);
    command(left, true); waitLines(left, 300);

    const auto pane = bounds(surface, left);
    const auto px = [dpi = GetDpiForWindow(surface)](int value) { return MulDiv(value, dpi, 96); };
    dragTab(app, 0, {pane.right - 10, (pane.top + pane.bottom) / 2});
    right = otherVisibleViewer(app.window, left);
    waitFor([&] { return IsWindowVisible(right) != FALSE; }, "Navigation split did not open");
    waitLines(right, 400); waitLines(left, 300);
    click(left, px(30), px(40)); command(left, false);
    waitFor([&] { return focused(right); }, "Back did not focus the right editor group");
    command(right, true);
    waitFor([&] { return focused(left); }, "Forward did not focus the left editor group");

    const auto terminals = surface;
    const auto first = FindWindowExW(terminals, nullptr, L"PoppyTerminal", nullptr);
    clickTerminalTab(app, first); addTerminal(app, first);
    const auto second = FindWindowExW(terminals, first, L"PoppyTerminal", nullptr);
    check(second && focused(second), "Adding a terminal did not focus its session");
    mouse(second, false);
    waitFor([&] { return focused(first) && IsWindowVisible(first) && !IsWindowVisible(second); }, "Back did not select the first terminal");
    command(first, false);
    waitFor([&] { return focused(left); }, "Back did not move from Terminal to Editor");
    controlShortcut(app, left, 'J', [&] { return !IsWindowVisible(first); }, "Cannot hide Terminal for navigation");
    command(left, true);
    waitFor([&] { return IsWindowVisible(first) && focused(first); }, "Forward did not reveal and focus Terminal");
    command(first, true);
    waitFor([&] { return focused(second); }, "Forward lost the second terminal");
    check(PostMessageW(terminals, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(terminals), MAKELPARAM(-1, -1)), "Cannot open the terminal menu");
    waitFor([&] { GUITHREADINFO info{}; info.cbSize = sizeof(info); return GetGUIThreadInfo(app.process.dwThreadId, &info) && (info.flags & GUI_INMENUMODE); }, "Terminal menu did not open");
    PostMessageW(terminals, WM_KEYDOWN, VK_END, 0); PostMessageW(terminals, WM_KEYDOWN, VK_RETURN, 0);
    waitFor([&] { return !IsWindow(second); }, "Cannot close the second terminal");
    command(first, false);
    waitFor([&] { return focused(left); }, "Back did not skip the closed terminal and duplicate current session");

    const auto path = other.root.native();
    COPYDATASTRUCT data{0x50505901, static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(path.c_str())};
    check(send(app.host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)), "Cannot add a navigation project");
    const auto otherTree = app.tree(), otherSurface = app.window;
    const auto otherViewer = FindWindowExW(otherSurface, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(otherTree, treeGetCount) == 2; }, "Second navigation project did not load");
    send(otherTree, treeSelect, nextItem(otherTree, TreeRelation::Child, nextItem(otherTree, TreeRelation::Root)));
    waitLines(otherViewer, 500); mouse(otherViewer, false);
    waitFor([&] { return IsWindowVisible(surface) && focused(left); }, "Back did not return to the previous project's editor");
    command(left, true);
    waitFor([&] { return IsWindowVisible(otherSurface) && focused(otherViewer); }, "Forward did not restore the next project's editor");
    command(otherViewer, false);
    waitFor([&] { return IsWindowVisible(surface) && focused(left); }, "Cannot return before closing a project");
    click(app.host, px(60), px(44 + projectRowHeight + 24), true);
    check(!IsWindow(otherSurface), "Navigation project did not close");
    command(left, true);
    check(IsWindowVisible(surface) && focused(left), "Forward reopened a closed project");
}
void previewTabTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 3; ++i) writeLines(fixture.root / (std::wstring(1, L'a' + i) + L".txt"), 200 + i * 100);
    {
        Application app(executable, fixture.root);
        const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitFor([&] { return send(tree, treeGetCount) == 4; }, "Preview fixture did not load");
        const auto a = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        const auto b = nextItem(tree, TreeRelation::Next, a), c = nextItem(tree, TreeRelation::Next, b);
        send(tree, treeSelect, a); waitLines(viewer, 200);
        send(viewer, WM_VSCROLL, SB_PAGEDOWN);
        send(tree, treeSelect, b); waitLines(viewer, 300);
        check(visibleTabCount(app) == 1 && scrollInfo(viewer).nPos == 0, "A new file did not replace the preview with a fresh view");
        clickTab(app, 0);
        send(tree, treeSelect, c); waitLines(viewer, 400);
        check(visibleTabCount(app) == 1, "Single-clicking a preview made it permanent");
        // Replace requests before their results arrive, then keep the final tab.
        send(tree, treeSelect, a); send(tree, treeSelect, b); send(tree, treeSelect, a); waitLines(viewer, 200);
        check(visibleTabCount(app) == 1, "Pending loads accumulated previews");
        keepTab(app, 0);
        send(viewer, WM_VSCROLL, SB_PAGEDOWN); const auto top = scrollInfo(viewer).nPos;
        send(tree, treeSelect, b); waitLines(viewer, 300);
        check(visibleTabCount(app) == 2, "Double-clicking did not keep a tab open");
        clickTab(app, 0); waitLines(viewer, 200);
        check(scrollInfo(viewer).nPos == top, "Keeping a preview lost its scroll position");
        send(tree, treeSelect, c); waitLines(viewer, 400);
        check(visibleTabCount(app) == 2, "Opening from a permanent tab did not reuse the inactive preview");
        send(viewer, WM_CHAR, '!');
        send(tree, treeSelect, b); waitLines(viewer, 300);
        check(visibleTabCount(app) == 3, "Opening another file replaced an edited preview");
        clickTab(app, 1); waitLines(viewer, 400);
        check(AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, TRUE), "Cannot attach preview Undo input");
        BYTE previous[256]{}, keys[256]{}; GetKeyboardState(previous); keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        SetKeyboardState(keys); send(viewer, WM_KEYDOWN, 'Z'); SetKeyboardState(previous);
        AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, FALSE);
        send(tree, treeSelect, b); waitLines(viewer, 300);
    }
    {
        Application app(executable, fixture.root); const auto tree = app.tree();
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitLines(viewer, 200);
        check(visibleTabCount(app) == 2, "Restart restored a preview or lost a kept/edited tab");
        clickTab(app, 1); waitLines(viewer, 400);
        const auto a = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        send(tree, treeSelect, nextItem(tree, TreeRelation::Next, a)); waitLines(viewer, 300);
        toggleTabPin(app, 2);
        send(tree, treeSelect, a); waitLines(viewer, 200);
        check(visibleTabCount(app) == 3, "Pinning did not keep the preview");
    }
}
void editorTailTests(const fs::path& executable) {
    Fixture fixture;
    const auto path = fixture.root / L"output.log";
    writeLines(path, 200);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 2; }, "Tail fixture did not load");
    send(tree, treeSelect, nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root)));
    waitLines(viewer, 200);
    const auto atBottom = [&] {
        const auto info = scrollInfo(viewer);
        return info.nPos == std::max(0, info.nMax - static_cast<int>(info.nPage) + 1);
    };
    const auto append = [&] {
        std::ofstream file(path, std::ios::app);
        for (int i = 0; i < 100; ++i) file << "appended entry\n";
        file.close(); check(!file.fail(), "Cannot append to the tail fixture");
    };
    send(viewer, WM_VSCROLL, SB_BOTTOM);
    append(); waitLines(viewer, 300);
    check(atBottom(), "The file watcher reloaded an append without following the tail");
    send(viewer, WM_VSCROLL, SB_LINEUP); const auto paused = scrollInfo(viewer).nPos;
    append(); waitLines(viewer, 400);
    check(scrollInfo(viewer).nPos == paused && !atBottom(), "The file watcher moved a viewport that was scrolled up");
    send(viewer, WM_VSCROLL, SB_BOTTOM);
    append(); waitLines(viewer, 500);
    check(atBottom(), "File watcher tail following did not resume at the bottom");
    writeLines(path, 10); waitLines(viewer, 10);
    check(atBottom() && scrollInfo(viewer).nPos == 10, "File truncation left tail following outside the file");
    append(); waitLines(viewer, 110);
    check(atBottom(), "Growing a short file did not keep following the tail");
    send(viewer, WM_CHAR, '!');
    append(); send(app.window, WM_KEYDOWN, VK_F5);
    // Confirm completion of F5 independently of the dirty editor's row count.
    writeFile(fixture.root / L"refresh-marker.txt");
    waitFor([&] { return send(tree, treeGetCount) == 3; }, "The dirty tail fixture did not refresh");
    check(lastTextRow(viewer) == 110, "Tail following replaced unsaved text with an external append");
    // Let the application close without saving over the external append.
    check(PostMessageW(app.host, WM_CLOSE, 0, 0) != FALSE, "Could not close the tail test");
    answerTabSave(app, L"output.log", IDNO);
    waitFor([&] { return WaitForSingleObject(app.process.hProcess, 0) == WAIT_OBJECT_0; }, "The tail test did not close");
}
void explorerPreviewTabTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 3; ++i) writeLines(fixture.root / (std::wstring(1, L'a' + i) + L".txt"), 200 + i * 100);
    {
        Application app(executable, fixture.root);
        const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitFor([&] { return send(tree, treeGetCount) == 4; }, "Explorer preview fixture did not load");
        const auto a = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        const auto b = nextItem(tree, TreeRelation::Next, a), c = nextItem(tree, TreeRelation::Next, b);
        const auto clickFile = [&](TreeItem item, bool keep = false) {
            const int height = static_cast<int>(send(tree, treeGetRowHeight));
            const int y = (static_cast<int>(send(tree, treeGetRow, item)) - scrollInfo(tree).nPos) * height + height / 2;
            const int x = MulDiv(60, GetDpiForWindow(app.window), 96);
            click(tree, x, y);
            if (keep) {
                send(tree, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x, y));
                send(tree, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            }
        };
        clickFile(a); waitLines(viewer, 200);
        send(viewer, WM_VSCROLL, SB_PAGEDOWN); const auto top = scrollInfo(viewer).nPos;
        clickFile(a, true);
        check(visibleTabCount(app) == 1 && scrollInfo(viewer).nPos == top,
              "Double-clicking an Explorer preview duplicated it or lost its scroll position");
        clickFile(b); waitLines(viewer, 300);
        check(visibleTabCount(app) == 2, "Double-clicking in Explorer did not keep the preview");
        clickFile(a, true); waitLines(viewer, 200);
        check(visibleTabCount(app) == 2 && scrollInfo(viewer).nPos == top,
              "Double-clicking a permanent Explorer file duplicated it or lost its view");
        clickFile(c); waitLines(viewer, 400);
        check(visibleTabCount(app) == 2, "Single-clicking in Explorer did not reuse the inactive preview");
        clickFile(b); clickFile(c, true); clickFile(b); waitLines(viewer, 300);
        check(visibleTabCount(app) == 3, "Double-clicking in Explorer failed to keep a pending preview");
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitLines(viewer, 200);
        check(visibleTabCount(app) == 2, "Restart lost an Explorer-kept tab or restored a preview");
        clickTab(app, 1); waitLines(viewer, 400);
    }
}
void previewTabSplitTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 3; ++i) writeLines(fixture.root / (std::wstring(1, L'a' + i) + L".txt"), 200 + i * 100);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), left = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    HWND right = nullptr;
    waitFor([&] { return send(tree, treeGetCount) == 4; }, "Split preview fixture did not load");
    const auto a = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    const auto b = nextItem(tree, TreeRelation::Next, a), c = nextItem(tree, TreeRelation::Next, b);
    send(tree, treeSelect, a); waitLines(left, 200); keepTab(app, 0);
    send(tree, treeSelect, b); waitLines(left, 300);
    auto body = bounds(app.window, left);
    dragTab(app, 1, {body.right - 10, (body.top + body.bottom) / 2});
    right = otherVisibleViewer(app.window, left);
    waitLines(left, 200); waitLines(right, 300);
    clickTab(app, 0);
    send(tree, treeSelect, c); waitLines(left, 400);
    check(visibleTabCount(app) == 3 && lastTextRow(right) == 300, "Opening a left preview replaced the right preview");
    body = bounds(app.window, right);
    dragTab(app, 2, {(body.left + body.right) / 2, (body.top + body.bottom) / 2});
    waitLines(left, 200); waitLines(right, 400);
    check(visibleTabCount(app) == 2, "Moving a preview left two previews in its destination group");
    send(tree, treeSelect, b); waitLines(right, 300);
    check(visibleTabCount(app) == 2, "Dragging a preview made it permanent");
}
void htmlTabToolsTests(const fs::path& executable) {
    Fixture fixture;
    writeLines(fixture.root / L"a.html", 200); writeLines(fixture.root / L"b.HTM", 300); writeLines(fixture.root / L"c.txt", 400);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), left = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    HWND right = nullptr;
    waitFor([&] { return send(tree, treeGetCount) == 4; }, "HTML tab fixture did not load");
    const UINT dpi = GetDpiForWindow(app.window);
    const auto selectMode = [&](HWND viewer, bool preview, UINT message = WM_LBUTTONDOWN) {
        const auto tools = editorModeTools(app, viewer, 160);
        const auto point = MAKELPARAM(tools.right - (tools.right - tools.left) * (preview ? 1 : 3) / 4, (tools.top + tools.bottom) / 2);
        send(app.window, message, MK_LBUTTON, point); send(app.window, WM_LBUTTONUP, 0, point);
    };
    const auto waitSource = [&](HWND viewer, int lines) {
        // Opening HTML now starts in Preview; wait for the document before
        // selecting Source, including when the asynchronous load is pending.
        waitFor([&] { selectMode(viewer, false); return lastTextRow(viewer) == lines; },
            "The HTML tab did not switch to Source");
    };
    auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    for (int i = 0; i < 3; ++i) {
        send(tree, treeSelect, item);
        if (i < 2) waitSource(left, 200 + i * 100); else waitLines(left, 400);
        keepTab(app, i);
        item = nextItem(tree, TreeRelation::Next, item);
    }
    const auto switchMode = [&](HWND viewer, bool preview, UINT message = WM_LBUTTONDOWN) {
        const auto rect = bounds(app.window, viewer);
        const auto tools = editorModeTools(app, viewer, 160);
        selectMode(viewer, preview, message);
        GUITHREADINFO input{}; input.cbSize = sizeof(input);
        check(GetGUIThreadInfo(app.process.dwThreadId, &input) && input.hwndCapture != app.window && visibleTabCount(app) == 3,
            "HTML mode tools started a tab drag or changed the tabs");
        const auto after = bounds(app.window, viewer);
        check(EqualRect(&rect, &after), "HTML mode switching moved the viewer below a toolbar");
        for (int i = 0; i < visibleTabCount(app); ++i) {
            const auto tab = tabBounds(app, i);
            if (tab.left >= rect.left && tab.left < rect.right && tab.top == tools.top)
                check(tab.right <= tools.left, "HTML tools overlapped a bottom-row tab");
        }
    };
    clickTab(app, 0); waitSource(left, 200);
    check(bounds(app.window, left).top == tabBounds(app, 0).bottom, "HTML source did not start immediately below the tabs");
    switchMode(left, true); waitFor([&] { return scrollInfo(left).nMax == 0; }, "Tab Preview did not activate HTML preview");
    switchMode(left, false); waitLines(left, 200);
    switchMode(left, true, WM_LBUTTONDBLCLK); waitFor([&] { return scrollInfo(left).nMax == 0; }, "Double clicking Preview did not select preview mode");
    switchMode(left, false); waitLines(left, 200);
    // A pinned row forces multiple rows while keeping the selected HTML at
    // the top. Its mode buttons must still live alongside the bottom tabs.
    toggleTabPin(app, 0);
    check(tabBounds(app, 0).top < tabBounds(app, 1).top, "HTML tools fixture did not create a pinned row");
    const auto tools = editorModeTools(app, left, 160);
    check(tools.top == tabBounds(app, 2).top, "HTML tools did not follow the bottom tab row");
    click(app.window, tools.right - MulDiv(58, dpi, 96) - (tools.right - tools.left) / 4, tabBounds(app, 0).top + MulDiv(17, dpi, 96));
    check(lastTextRow(left) == 200 && visibleTabCount(app) == 3, "HTML mode tools retained their old top-row hit region");
    switchMode(left, true); waitFor([&] { return scrollInfo(left).nMax == 0; }, "Bottom-row Preview did not activate HTML preview");
    switchMode(left, false); waitLines(left, 200);
    toggleTabPin(app, 0);
    clickTab(app, 2); waitLines(left, 400);
    // Ordinary files do not retain the HTML buttons from the previous tab.
    auto body = bounds(app.window, left);
    click(app.window, body.right - MulDiv(70, dpi, 96), MulDiv(17, dpi, 96));
    check(lastTextRow(left) == 400 && visibleTabCount(app) == 3, "An ordinary file retained HTML tools");
    clickTab(app, 0); waitSource(left, 200);
    dragTab(app, 1, {body.right - 10, (body.top + body.bottom) / 2});
    right = otherVisibleViewer(app.window, left);
    waitSource(left, 200); waitSource(right, 300);
    SetWindowPos(app.host, nullptr, 0, 0, MulDiv(1000, dpi, 96), MulDiv(700, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    send(app.window, WM_NULL);
    // Each pane owns its mode even while focus is in the other pane.
    switchMode(right, true); check(scrollInfo(right).nMax == 0 && lastTextRow(left) == 200, "Right Preview changed the left pane");
    switchMode(left, true); check(scrollInfo(left).nMax == 0 && scrollInfo(right).nMax == 0, "Left Preview changed the right pane");
    switchMode(right, false); waitLines(right, 300); check(scrollInfo(left).nMax == 0, "Right Source closed the left preview");
    switchMode(left, false); waitLines(left, 200);
}
void bulkTabCloseTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 4; ++i) writeLines(fixture.root / (std::wstring(1, L'a' + i) + L".txt"), 200 + i * 100);
    {
        Application app(executable, fixture.root);
        const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitFor([&] { return send(tree, treeGetCount) == 5; }, "Bulk close fixture did not load");
        auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        TreeItem third = 0;
        for (int i = 0; i < 4; ++i) {
            if (i == 2) third = item;
            send(tree, treeSelect, item); waitLines(viewer, 200 + i * 100); keepTab(app, i);
            item = nextItem(tree, TreeRelation::Next, item);
        }
        toggleTabPin(app, 0); toggleTabPin(app, 1);
        clickTab(app, 2); waitLines(viewer, 400);
        send(viewer, WM_VSCROLL, SB_LINEDOWN); const auto top = scrollInfo(viewer).nPos;
        clickTab(app, 3); waitLines(viewer, 500);
        tabMenuCommand(app, 2, TabMenuCommand::CloseOthers); waitLines(viewer, 400);
        check(visibleTabCount(app) == 3 && scrollInfo(viewer).nPos == top,
              "Close Others lost its inactive target, pinned tabs or scroll position");
        tabMenuCommand(app, 0, TabMenuCommand::CloseOthers); waitLines(viewer, 300);
        check(visibleTabCount(app) == 2, "Close Others on a pinned tab did not preserve all pinned tabs");
        send(tree, treeSelect, third); waitLines(viewer, 400);
        tabMenuCommand(app, 0, TabMenuCommand::CloseAll); waitLines(viewer, 300);
        check(visibleTabCount(app) == 2, "Close All closed a pinned tab");
        clickTab(app, 0); waitLines(viewer, 200);
        clickTab(app, 1); waitLines(viewer, 300);
        toggleTabPin(app, 0); toggleTabPin(app, 0);
        tabMenuCommand(app, 0, TabMenuCommand::CloseAll);
        check(visibleTabCount(app) == 0 && scrollInfo(viewer).nMax == 0, "Close All did not clear the last regular tabs");
        // Repeat while the reopened document may still be loading.
        send(tree, WM_KEYDOWN, VK_RETURN);
        tabMenuCommand(app, 0, TabMenuCommand::CloseAll);
        check(visibleTabCount(app) == 0 && scrollInfo(viewer).nMax == 0, "A pending load survived Close All");
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        check(visibleTabCount(app) == 0 && scrollInfo(viewer).nMax == 0, "Restart restored bulk-closed tabs");
    }
}
void bulkTabSaveTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 4; ++i) writeLines(fixture.root / (std::wstring(1, L'a' + i) + L".txt"), 200 + i * 100);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 5; }, "Bulk save fixture did not load");
    auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    for (int i = 0; i < 4; ++i) {
        send(tree, treeSelect, item); waitLines(viewer, 200 + i * 100);
        if (i < 3) send(viewer, WM_CHAR, '!');
        item = nextItem(tree, TreeRelation::Next, item);
    }
    toggleTabPin(app, 0);
    tabMenuCommand(app, 3, TabMenuCommand::CloseOthers, false);
    answerTabSave(app, L"b.txt", IDNO); answerTabSave(app, L"c.txt", IDCANCEL);
    send(app.window, WM_NULL);
    check(visibleTabCount(app) == 4 && lastTextRow(viewer) == 500,
          "Cancel after a discard decision partially closed the tabs");
    check(readText(fixture.root / L"b.txt").front() != '!', "Discard or Cancel unexpectedly saved edits");

    // A conflicting external edit makes the second save fail after the first
    // succeeds. All tabs and the remaining unsaved buffers must stay open.
    writeLines(fixture.root / L"c.txt", 600); const auto external = readText(fixture.root / L"c.txt");
    tabMenuCommand(app, 0, TabMenuCommand::CloseAll, false);
    answerTabSave(app, L"b.txt", IDYES); answerTabSave(app, L"c.txt", IDYES);
    HWND error = nullptr;
    waitFor([&] {
        error = FindWindowW(L"#32770", L"Poppy — Save");
        DWORD pid = 0; if (error) GetWindowThreadProcessId(error, &pid);
        return error && pid == app.process.dwProcessId;
    }, "A failed bulk-close save did not report its error");
    send(error, WM_COMMAND, IDOK); send(app.window, WM_NULL);
    check(visibleTabCount(app) == 4, "A failed save partially closed the tabs");
    check(readText(fixture.root / L"b.txt").front() == '!', "Cancel lost the first unsaved buffer or Save did not persist it");
    check(readText(fixture.root / L"c.txt") == external, "A failed save overwrote external edits");
    clickTab(app, 2); waitLines(viewer, 400);
    tabMenuCommand(app, 0, TabMenuCommand::CloseAll, false);
    answerTabSave(app, L"c.txt", IDNO); send(app.window, WM_NULL); waitLines(viewer, 200);
    check(visibleTabCount(app) == 1 && readText(fixture.root / L"a.txt").front() != '!',
          "Close All prompted, saved or closed a dirty pinned tab");
    toggleTabPin(app, 0);
    tabMenuCommand(app, 0, TabMenuCommand::CloseAll, false);
    answerTabSave(app, L"a.txt", IDNO); send(app.window, WM_NULL);
    check(visibleTabCount(app) == 0, "Discard did not finish closing the last unpinned tab");
}
void bulkTabSplitTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 3; ++i) writeLines(fixture.root / (std::wstring(1, L'a' + i) + L".txt"), 200 + i * 100);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), left = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    HWND right = nullptr;
    waitFor([&] { return send(tree, treeGetCount) == 4; }, "Bulk split fixture did not load");
    auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    for (int i = 0; i < 3; ++i) {
        send(tree, treeSelect, item); waitLines(left, 200 + i * 100); keepTab(app, i);
        item = nextItem(tree, TreeRelation::Next, item);
    }
    const auto original = bounds(app.window, left);
    dragTab(app, 2, {original.right - 10, (original.top + original.bottom) / 2});
    right = otherVisibleViewer(app.window, left);
    waitLines(left, 300); waitLines(right, 400);
    send(right, WM_VSCROLL, SB_LINEDOWN); const auto top = scrollInfo(right).nPos;
    tabMenuCommand(app, 0, TabMenuCommand::CloseOthers); waitLines(left, 200); waitLines(right, 400);
    check(visibleTabCount(app) == 2 && IsWindowVisible(right) && scrollInfo(right).nPos == top,
          "Close Others affected the other editor group");
    // Close the left group while the right one has focus. It must migrate to
    // the remaining viewer without losing its document or position.
    clickTab(app, 1); waitLines(right, 400);
    tabMenuCommand(app, 0, TabMenuCommand::CloseAll); waitLines(right, 400);
    check(visibleTabCount(app) == 1 && !IsWindowVisible(left) && scrollInfo(right).nPos == top,
          "Close All crossed editor groups or failed to collapse the empty left group");
    tabMenuCommand(app, 0, TabMenuCommand::CloseAll);
    check(visibleTabCount(app) == 0, "Close All left an empty split document");
}
void tabTests(const fs::path& executable) {
    Fixture fixture;
    writeLines(fixture.root / L"a 日本語.txt", 200); writeLines(fixture.root / L"b.txt", 300);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    check(viewer != nullptr, "The viewer is missing");
    TreeItem first = 0, second = 0;
    waitFor([&] {
        first = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        second = nextItem(tree, TreeRelation::Next, first);
        return first && second;
    }, "The tab test files did not appear");
    const UINT dpi = GetDpiForWindow(app.window);
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const auto empty = bounds(app.window, viewer);
    send(tree, treeSelect, static_cast<WPARAM>(first)); waitLines(viewer, 200); keepTab(app, 0);
    const auto pane = bounds(app.window, viewer);
    check(pane.top == empty.top + px(34), "Opening a file did not make room for tabs");
    for (int i = 0; i < 12; ++i) { send(viewer, WM_VSCROLL, SB_LINEDOWN); send(viewer, WM_HSCROLL, SB_LINEDOWN); }
    const auto firstTop = scrollInfo(viewer).nPos, firstLeft = scrollInfo(viewer, SB_HORZ).nPos;
    check(firstTop > 0 && firstLeft == 0, "The wrapped document did not scroll vertically or scrolled horizontally");
    send(tree, treeSelect, static_cast<WPARAM>(second)); waitLines(viewer, 300);
    const auto firstBounds = tabBounds(app, 0), secondBounds = tabBounds(app, 1);
    check(firstBounds.right - firstBounds.left > secondBounds.right - secondBounds.left,
          "Tab widths did not follow Japanese and short file names");
    check(scrollInfo(viewer).nPos == 0 && scrollInfo(viewer, SB_HORZ).nPos == 0, "A new tab inherited another tab's scroll position");
    send(viewer, WM_VSCROLL, SB_PAGEDOWN); const auto secondTop = scrollInfo(viewer).nPos;
    const int tabY = pane.top - px(17);
    click(app.window, pane.left + px(30), tabY); waitLines(viewer, 200);
    check(scrollInfo(viewer).nPos == firstTop && scrollInfo(viewer, SB_HORZ).nPos == firstLeft, "Switching tabs lost the first tab's scroll position");
    // The tree still selects b.txt. Enter must reactivate its existing tab.
    send(tree, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
    check(scrollInfo(viewer).nPos == secondTop, "Reopening a file reset its existing tab");
    writeLines(fixture.root / L"a 日本語.txt", 220);
    click(app.window, pane.left + px(30), tabY); waitLines(viewer, 220);
    check(scrollInfo(viewer).nPos == firstTop, "Reloading a hidden tab lost its position");
    // Closing an inactive tab must keep the active document and position.
    clickTab(app, 1, false, true);
    check(lastTextRow(viewer) == 220 && scrollInfo(viewer).nPos == firstTop, "Closing an inactive tab changed the active document");
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top && scrollInfo(viewer).nMax == 0, "Closing the last tab left a duplicate tab or document");
    // Reopen, then close a newly loading tab and its neighbor. Older results
    // must not repopulate the viewer or replace the next selected document.
    send(tree, treeSelect, static_cast<WPARAM>(first)); waitLines(viewer, 220); keepTab(app, 0);
    send(tree, treeSelect, static_cast<WPARAM>(second));
    clickTab(app, 1, true);
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top, "Closing tabs during loading did not clear the tab strip");
    // b.txt is still selected in the tree, so reopening also exercises Enter.
    send(tree, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top && scrollInfo(viewer).nMax == 0, "A stale load restored a closed tab");
}
void rapidTabMiddleClickTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 4; ++i) writeLines(fixture.root / (std::wstring(1, L'a' + i) + L".txt"), 200 + i * 100);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 5; }, "Rapid middle-click fixture did not load");
    auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    for (int i = 0; i < 4; ++i) {
        send(tree, treeSelect, item); waitLines(viewer, 200 + i * 100);
        if (i < 3) keepTab(app, i);
        item = nextItem(tree, TreeRelation::Next, item);
    }
    send(viewer, WM_VSCROLL, SB_PAGEDOWN); const auto top = scrollInfo(viewer).nPos;
    const auto rect = tabBounds(app, 0);
    const auto point = MAKELPARAM(rect.left + MulDiv(16, GetDpiForWindow(app.window), 96), (rect.top + rect.bottom) / 2);
    int remaining = 4;
    // Keep clicking the same point as the next tab moves under the pointer.
    // Windows sends DOWN/UP/DBLCLK/UP for each pair of rapid middle-clicks.
    for (const auto message : {WM_MBUTTONDOWN, WM_MBUTTONDBLCLK, WM_MBUTTONDOWN, WM_MBUTTONDBLCLK}) {
        send(app.window, message, MK_MBUTTON, point);
        check(visibleTabCount(app) == --remaining, "A rapid middle-click did not close exactly one tab");
        send(app.window, WM_MBUTTONUP, 0, point);
        check(visibleTabCount(app) == remaining, "Releasing a middle-click closed another tab");
        if (remaining) check(lastTextRow(viewer) == 500 && scrollInfo(viewer).nPos == top,
                             "Closing inactive tabs rapidly changed the active preview or its scroll position");
    }
    check(scrollInfo(viewer).nMax == 0, "Rapid middle-clicks left the last preview open");
}
void pinnedTabTests(const fs::path& executable) {
    Fixture fixture;
    writeLines(fixture.root / L"a.txt", 200); writeLines(fixture.root / L"b.txt", 300); writeLines(fixture.root / L"c.txt", 400);
    {
        Application app(executable, fixture.root);
        const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitFor([&] { return send(tree, treeGetCount) == 4; }, "The pinned tab test files did not appear");
        auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        for (int lines : {200, 300, 400}) {
            send(tree, treeSelect, item); waitLines(viewer, lines); keepTab(app, visibleTabCount(app) - 1);
            item = nextItem(tree, TreeRelation::Next, item);
        }
        const auto singleRow = bounds(app.window, viewer);
        toggleTabPin(app, 1);
        check(lastTextRow(viewer) == 400, "Pinning an inactive tab changed the active file");
        const auto pinned = tabBounds(app, 0), regular = tabBounds(app, 1);
        check(pinned.bottom == regular.top && pinned.left == regular.left,
              "Pinned and regular tabs did not start on separate rows");
        check(bounds(app.window, viewer).top == singleRow.top + pinned.bottom - pinned.top,
              "The viewer did not reserve a dedicated pinned tab row");
        clickTab(app, 0); waitLines(viewer, 300);
        clickTab(app, 0, true);
        check(visibleTabCount(app) == 3 && lastTextRow(viewer) == 300, "Middle-click closed a pinned tab");
        const auto middlePoint = MAKELPARAM(pinned.left + MulDiv(16, GetDpiForWindow(app.window), 96), (pinned.top + pinned.bottom) / 2);
        send(app.window, WM_MBUTTONDBLCLK, MK_MBUTTON, middlePoint);
        send(app.window, WM_MBUTTONUP, 0, middlePoint);
        check(visibleTabCount(app) == 3 && lastTextRow(viewer) == 300, "Rapid middle-clicking closed a pinned tab");
        clickTab(app, 0, false, true);
        check(visibleTabCount(app) == 3 && lastTextRow(viewer) == 300, "A pinned tab retained a close button");
        toggleTabPin(app, 0);
        check(bounds(app.window, viewer).top == singleRow.top && tabBounds(app, 0).top == tabBounds(app, 1).top,
              "Unpinning the last pinned tab left an empty row");
        clickTab(app, 0, false, true); waitLines(viewer, 200);
        check(visibleTabCount(app) == 2, "An unpinned tab could not be closed");
        toggleTabPin(app, 1);
        check(lastTextRow(viewer) == 200, "Moving a pinned tab lost the active file");
        toggleTabPin(app, 1);
        check(tabBounds(app, 0).top == tabBounds(app, 1).top && bounds(app.window, viewer).top == singleRow.top,
              "Pinning the final regular tab left an empty regular row");
        clickTab(app, 0); waitLines(viewer, 400);
        clickTab(app, 1); waitLines(viewer, 200);
        send(viewer, WM_VSCROLL, SB_LINEDOWN);
        check(scrollInfo(viewer).nPos > 0, "The pinned test tab did not scroll");
    }
    {
        Application app(executable, fixture.root);
        app.tree();
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitLines(viewer, 200);
        check(scrollInfo(viewer).nPos > 0, "Restart lost a pinned tab's scroll position");
        check(tabBounds(app, 0).top == tabBounds(app, 1).top, "Restart split pinned tabs into separate rows");
        clickTab(app, 0, true); clickTab(app, 1, true);
        check(visibleTabCount(app) == 2, "Restart lost pinned tab protection");
        clickTab(app, 0); waitLines(viewer, 400);
        clickTab(app, 1); waitLines(viewer, 200);
        toggleTabPin(app, 0);
        check(lastTextRow(viewer) == 200, "Unpinning an inactive tab changed the active file");
        check(tabBounds(app, 0).bottom == tabBounds(app, 1).top,
              "Unpinning a tab did not move it below the pinned row");
        clickTab(app, 1, true);
        check(visibleTabCount(app) == 1 && lastTextRow(viewer) == 200, "Closing an unpinned neighbor changed the pinned file");
        check(bounds(app.window, viewer).top == tabBounds(app, 0).bottom,
              "Closing the final regular tab left an empty row");
        toggleTabPin(app, 0); clickTab(app, 0, true);
        check(visibleTabCount(app) == 0 && scrollInfo(viewer).nMax == 0, "The final unpinned tab could not be closed");
    }
}
void tabDragTests(const fs::path& executable) {
    Fixture fixture;
    writeLines(fixture.root / L"a 日本語の長い名前.txt", 200); writeLines(fixture.root / L"b.txt", 300);
    writeLines(fixture.root / L"c-longer-name.txt", 400); writeLines(fixture.root / L"d.txt", 500);
    int firstTop = 0, firstLeft = 0;
    {
        Application app(executable, fixture.root);
        const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
        check(SetWindowPos(app.window, nullptr, 0, 0, px(1280), px(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
              "Could not resize the drag test window");
        waitFor([&] { return send(tree, treeGetCount) == 5; }, "The drag test files did not appear");
        auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        for (int lines : {200, 300, 400, 500}) {
            send(tree, treeSelect, item); waitLines(viewer, lines); keepTab(app, visibleTabCount(app) - 1);
            item = nextItem(tree, TreeRelation::Next, item);
        }
        const auto before = [&](int index) { const auto r = tabBounds(app, index); return POINT{r.left + 1, (r.top + r.bottom) / 2}; };
        const auto after = [&](int index) { const auto r = tabBounds(app, index); return POINT{r.right - 1, (r.top + r.bottom) / 2}; };
        const auto checkOrder = [&](std::initializer_list<int> lines) {
            int index = 0;
            for (int count : lines) { clickTab(app, index++); waitLines(viewer, count); }
        };
        clickTab(app, 0); waitLines(viewer, 200);
        for (int i = 0; i < 5; ++i) { send(viewer, WM_VSCROLL, SB_LINEDOWN); send(viewer, WM_HSCROLL, SB_LINEDOWN); }
        firstTop = scrollInfo(viewer).nPos; firstLeft = scrollInfo(viewer, SB_HORZ).nPos;
        check(firstTop > 0 && firstLeft == 0, "The wrapped dragged document did not scroll vertically or scrolled horizontally");
        clickTab(app, 1); waitLines(viewer, 300);
        send(viewer, WM_VSCROLL, SB_PAGEDOWN); const auto secondTop = scrollInfo(viewer).nPos;
        dragTab(app, 0, after(2)); waitLines(viewer, 200);
        check(scrollInfo(viewer).nPos == firstTop && scrollInfo(viewer, SB_HORZ).nPos == firstLeft,
              "Dragging changed the active document's view state");
        checkOrder({300, 400, 200, 500});
        dragTab(app, 2, before(0)); waitLines(viewer, 200);
        clickTab(app, 1); waitLines(viewer, 300);
        check(scrollInfo(viewer).nPos == secondTop, "Dragging lost an inactive tab's view state");
        // Cancellation, loss of capture, and releasing outside the strip must
        // leave the original ordering intact and end the mouse interaction.
        for (UINT cancel : {WM_CANCELMODE, WM_CAPTURECHANGED}) {
            dragTab(app, 0, after(3), cancel); checkOrder({200, 300, 400, 500});
        }
        const auto first = tabBounds(app, 0);
        dragTab(app, 0, {first.left - px(10), (first.top + first.bottom) / 2});
        checkOrder({200, 300, 400, 500});
        beginTabDrag(app, 0, after(3));
        check(PostMessageW(viewer, WM_KEYDOWN, VK_ESCAPE, 0) != FALSE, "Could not cancel a tab drag with Escape");
        waitFor([&] {
            GUITHREADINFO info{}; info.cbSize = sizeof(info);
            check(GetGUIThreadInfo(app.process.dwThreadId, &info) != FALSE, "Could not read drag capture");
            return info.hwndCapture == nullptr;
        }, "Escape did not release tab drag capture");
        check(!IsWindowVisible(FindWindowExW(app.window, nullptr, L"PoppyDropPreview", nullptr)) &&
              !draggedTab(app), "Escape left a drag overlay visible");
        send(app.window, WM_LBUTTONUP); checkOrder({200, 300, 400, 500});
        auto empty = after(3); empty.x += px(20);
        dragTab(app, 0, empty); checkOrder({300, 400, 500, 200});
        toggleTabPin(app, 0); toggleTabPin(app, 1);
        dragTab(app, 0, after(1)); checkOrder({400, 300, 500, 200});
        dragTab(app, 3, before(2)); checkOrder({400, 300, 200, 500});
        // The two groups remain separate, including when a drag crosses rows.
        dragTab(app, 0, after(3)); checkOrder({400, 300, 200, 500});
        dragTab(app, 2, before(0)); waitLines(viewer, 200);
        check(tabBounds(app, 0).top == tabBounds(app, 1).top && tabBounds(app, 1).bottom == tabBounds(app, 2).top,
              "Dragging mixed pinned and regular tab rows");
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitLines(viewer, 200);
        check(scrollInfo(viewer).nPos == firstTop && scrollInfo(viewer, SB_HORZ).nPos == firstLeft,
              "Restart lost the dragged tab's selection or view state");
        int index = 0;
        for (int lines : {400, 300, 200, 500}) { clickTab(app, index++); waitLines(viewer, lines); }
        clickTab(app, 0, true); clickTab(app, 1, true);
        check(visibleTabCount(app) == 4, "Dragging lost the saved pin state");
    }
}
void wrappingTabDragTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 8; ++i)
        writeLines(fixture.root / (L"tab-" + std::to_wstring(i) + L"-" + std::wstring(12 + i % 3 * 3, L'x') + L".txt"), 100 + i);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
    check(SetWindowPos(app.window, nullptr, 0, 0, px(820), px(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not resize the wrapped drag test window");
    waitFor([&] { return send(tree, treeGetCount) == 9; }, "The wrapped drag test files did not appear");
    auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    for (int i = 0; i < 8; ++i) {
        send(tree, treeSelect, item); waitLines(viewer, 100 + i); keepTab(app, i);
        item = nextItem(tree, TreeRelation::Next, item);
    }
    const auto first = tabBounds(app, 0), last = tabBounds(app, 7);
    check(first.top < last.top, "The drag test tabs did not wrap");
    tabDragPreview(app, 0, viewer);
    dragTab(app, 7, {first.left + 1, (first.top + first.bottom) / 2}); waitLines(viewer, 107);
    clickTab(app, 0); waitLines(viewer, 107);
    for (int i = 1; i < 8; ++i) { clickTab(app, i); waitLines(viewer, 99 + i); }
    check(SetWindowPos(app.window, nullptr, 0, 0, px(820), px(480), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not shorten the wrapped drag test window");
    const auto pane = bounds(app.window, viewer);
    const int y = pane.top - px(17);
    for (int i = 0; i < 8; ++i) click(app.window, pane.right - px(42), y);
    clickTab(app, 0); waitLines(viewer, 107);
    const POINT next{pane.right - px(14), y};
    beginTabDrag(app, 0, next);
    const auto ghost = draggedTab(app);
    const auto beforeScroll = bounds(app.window, ghost);
    // Fire the repeat timer while hovering over the next-row button. The
    // source tab must stay captured even after its row is no longer visible.
    for (int i = 0; i < 8; ++i) send(app.window, WM_TIMER, 2);
    const auto afterScroll = bounds(app.window, ghost);
    check(EqualRect(&beforeScroll, &afterScroll), "Scrolling the source strip moved or resized the dragged tab");
    const auto target = tabBounds(app, visibleTabCount(app) - 1);
    const POINT drop{target.right - 1, (target.top + target.bottom) / 2};
    send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(drop.x, drop.y));
    send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(drop.x, drop.y));
    clickTab(app, visibleTabCount(app) - 1); waitLines(viewer, 107);
    for (int i = 0; i < 8; ++i) click(app.window, pane.right - px(42), y);
    clickTab(app, 0); waitLines(viewer, 100);
    check(SetWindowPos(app.window, nullptr, 0, 0, px(820), px(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not restore the wrapped drag test window");
    for (int i = 0; i < 8; ++i) { clickTab(app, i); waitLines(viewer, 100 + i); }
}
void wrappingTabTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 8; ++i)
        writeLines(fixture.root / (L"tab-" + std::to_wstring(i) + L"-" + std::wstring(12 + i % 3 * 3, L'x') + L".txt"), 100 + i);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    check(viewer != nullptr, "The viewer is missing");
    const UINT dpi = GetDpiForWindow(app.window);
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    check(SetWindowPos(app.window, nullptr, 0, 0, px(820), px(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not resize the tab test window");
    const auto empty = bounds(app.window, viewer);
    const int rowHeight = px(34);
    const auto checkRows = [&](int count) {
        const auto pane = bounds(app.window, viewer);
        check(visibleTabCount(app) == count, "Wrapped tabs are missing");
        RECT previous{pane.left, empty.top, pane.left, empty.top};
        for (int i = 0; i < count; ++i) {
            const auto rect = tabBounds(app, i);
            check(rect.left >= pane.left && rect.right <= pane.right && rect.bottom - rect.top == rowHeight,
                  "A tab extends outside its row");
            if (!i || rect.top == previous.top)
                check(rect.left == previous.right && rect.top == previous.top, "Tabs overlap or leave a gap in their row");
            else {
                check(rect.left == pane.left && rect.top == previous.bottom, "Tabs did not wrap to the next row");
                check(rect.right - rect.left > pane.right - previous.right, "A tab wrapped before the row was full");
            }
            previous = rect;
        }
        check(pane.top == previous.bottom, "The viewer did not reserve the wrapped tab rows");
    };
    waitFor([&] { return send(tree, treeGetCount) == 9; }, "The wrapping test files did not appear");
    auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    TreeItem lastRemaining = 0;
    for (int i = 0; i < 8; ++i) {
        check(item != 0, "A tab test tree item is missing");
        send(tree, treeSelect, static_cast<WPARAM>(item)); waitLines(viewer, 100 + i); keepTab(app, i);
        checkRows(i + 1);
        if (i == 6) lastRemaining = item;
        item = nextItem(tree, TreeRelation::Next, item);
    }
    check(bounds(app.window, viewer).bottom == empty.bottom, "Wrapping tabs moved the terminal boundary");
    const auto shortTab = tabBounds(app, 0), longTab = tabBounds(app, 2);
    check(shortTab.right - shortTab.left < longTab.right - longTab.left, "Wrapped tabs all have the same width");
    for (int i = 0; i < 8; ++i) { clickTab(app, i); waitLines(viewer, 100 + i); }
    const auto wrapped = bounds(app.window, viewer);
    click(app.window, wrapped.right - px(3), empty.top + px(17));
    check(lastTextRow(viewer) == 107, "Clicking the empty part of a tab row changed the active file");
    // Close an inactive tab in the third row, then the active last-row tab.
    clickTab(app, 5, true);
    check(lastTextRow(viewer) == 107, "Closing a wrapped inactive tab changed the document");
    clickTab(app, 6); waitLines(viewer, 107);
    clickTab(app, 6, false, true); waitLines(viewer, 106);
    checkRows(6);
    const auto fewer = bounds(app.window, viewer);
    check(fewer.top < wrapped.top, "Closing a wrapped tab did not remove the empty row");
    check(SetWindowPos(app.window, nullptr, 0, 0, px(1280), px(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not widen the tab test window");
    checkRows(6);
    check(bounds(app.window, viewer).top < fewer.top, "Widening the viewer did not reduce the tab rows");
    clickTab(app, 0); waitLines(viewer, 100); clickTab(app, 5); waitLines(viewer, 106);
    // A short window must keep the document visible and allow row navigation.
    check(SetWindowPos(app.window, nullptr, 0, 0, px(820), px(480), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not shorten the tab test window");
    const auto pane = bounds(app.window, viewer); const int tabY = pane.top - px(17);
    check(pane.top == empty.top + rowHeight && pane.bottom > pane.top, "Many tabs hid the document in a short window");
    for (int i = 0; i < 8; ++i) click(app.window, pane.right - px(42), tabY);
    check(lastTextRow(viewer) == 106, "Scrolling the tab rows changed the active file");
    clickTab(app, 0); waitLines(viewer, 100);
    for (int i = 0; i < 8; ++i) click(app.window, pane.right - px(14), tabY);
    clickTab(app, visibleTabCount(app) - 1); waitLines(viewer, 106);
    click(app.window, pane.right - px(42), tabY);
    // Reactivating an existing file must reveal its row without adding a tab.
    send(tree, treeSelect, static_cast<WPARAM>(lastRemaining)); waitLines(viewer, 106);
    clickTab(app, visibleTabCount(app) - 1, true); waitLines(viewer, 104);
    check(SetWindowPos(app.window, nullptr, 0, 0, px(820), px(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not restore the tab test window");
    checkRows(5);
    clickTab(app, 0); waitLines(viewer, 100); clickTab(app, 4); waitLines(viewer, 104);
}
void createItemTests(const fs::path& executable) {
    Fixture fixture; fs::create_directory(fixture.root / L"collapsed 日本語"); writeFile(fixture.root / L"collapsed 日本語" / L"existing.txt");
    Application app(executable, fixture.root); const auto tree = app.tree();
    waitFor([&] { return send(tree, treeGetCount) == 2; }, "Creation fixture did not load");
    const auto root = nextItem(tree, TreeRelation::Root), folder = nextItem(tree, TreeRelation::Child, root);
    const auto start = [&](TreeItem target, bool directory, const wchar_t* name, TreeItem parent = 0) {
        send(tree, treeSelect, parent ? parent : target);
        POINT screen{-1, -1};
        if (parent) {
            const auto scroll = scrollInfo(tree);
            const int height = static_cast<int>(send(tree, treeGetRowHeight));
            screen = {MulDiv(70, GetDpiForWindow(tree), 96), static_cast<LONG>((send(tree, treeGetRow, target) - scroll.nPos) * height + height / 2)};
            check(ClientToScreen(tree, &screen), "Cannot locate the file context menu");
        } else parent = target;
        check(PostMessageW(tree, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(tree), MAKELPARAM(screen.x, screen.y)), "Cannot open creation context menu");
        const auto menuOpen = [&] { GUITHREADINFO info{}; info.cbSize = sizeof(info); check(GetGUIThreadInfo(app.process.dwThreadId, &info), "Cannot inspect creation menu"); return (info.flags & GUI_INMENUMODE) != 0; };
        waitFor(menuOpen, "Creation context menu did not open");
        PostMessageW(tree, WM_KEYDOWN, VK_HOME, 0);
        if (directory) PostMessageW(tree, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(tree, WM_KEYDOWN, VK_RETURN, 0);
        waitFor([&] { return !menuOpen(); }, "Creation context menu did not close");
        HWND edit = nullptr;
        waitFor([&] { edit = FindWindowExW(tree, nullptr, treeInputClassName, nullptr); return edit != nullptr; }, "Creation did not show the name input");
        const auto inputBounds = bounds(tree, edit);
        const auto height = send(tree, treeGetRowHeight), targetRow = send(tree, treeGetRow, target);
        SCROLLINFO scroll{sizeof(scroll), SIF_POS, 0, 0, 0, 0, 0}; check(GetScrollInfo(tree, SB_VERT, &scroll), "Cannot inspect creation scroll position");
        check(expanded(tree, parent) && inputBounds.top == (targetRow + 1 - scroll.nPos) * height,
              "Creation input did not appear on the row immediately after the context item");
        int depth = 0;
        for (auto ancestor = parent; ancestor; ancestor = nextItem(tree, TreeRelation::Parent, ancestor)) ++depth;
        const auto dpi = GetDpiForWindow(tree);
        check(inputBounds.left == (depth + 1) * MulDiv(18, dpi, 96) + MulDiv(5, dpi, 96), "Creation input did not use the child's indentation");
        check(SetWindowTextW(edit, name), "Cannot enter new item name"); return edit;
    };
    const auto fail = [&](bool directory, const wchar_t* name) {
        const auto edit = start(root, directory, name);
        PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        HWND dialog = nullptr; std::pair<DWORD, HWND*> context{app.process.dwProcessId, &dialog};
        waitFor([&] { EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto& context = *reinterpret_cast<std::pair<DWORD, HWND*>*>(data); DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
            wchar_t type[32]{}; GetClassNameW(window, type, 32);
            if (pid == context.first && std::wstring(type) == L"#32770") { *context.second = window; return FALSE; } return TRUE;
        }, reinterpret_cast<LPARAM>(&context)); return dialog != nullptr; }, "Invalid creation did not report an error");
        send(dialog, WM_COMMAND, IDOK);
    };
    auto edit = start(folder, false, L"新規 ファイル.txt"); send(edit, WM_KEYDOWN, VK_RETURN);
    const auto created = fixture.root / L"collapsed 日本語" / L"新規 ファイル.txt";
    waitFor([&] { return fs::is_regular_file(created); }, "New File did not create the file inside a collapsed folder");
    check(fs::file_size(created) == 0, "New File did not create an empty file");
    waitFor([&] { return expanded(tree, folder) && send(tree, treeGetCount) == 4; }, "Creation did not expand and refresh the parent");
    check(nextItem(tree, TreeRelation::Parent, static_cast<TreeItem>(send(tree, treeGetSelection))) == folder, "Created file was not selected");
    const auto createdFile = static_cast<TreeItem>(send(tree, treeGetSelection));
    check(visibleTabCount(app) == 1, "Created file did not open in the editor");
    keepTab(app, 0);
    edit = start(folder, true, L"空 フォルダー"); send(edit, WM_KEYDOWN, VK_RETURN);
    waitFor([&] { return fs::is_directory(created.parent_path() / L"空 フォルダー") && send(tree, treeGetCount) == 5; }, "New Folder did not create and display a folder");
    check(visibleTabCount(app) == 1, "Creating a folder opened a file tab");
    writeFile(created.parent_path() / L"空 フォルダー" / L"child.txt");
    controlKey(app, tree, 'Z');
    waitFor([&] { return !fs::exists(created.parent_path() / L"空 フォルダー"); }, "Undo did not remove a created folder and its children");
    controlKey(app, tree, 'Y');
    waitFor([&] { return fs::exists(created.parent_path() / L"空 フォルダー" / L"child.txt") && send(tree, treeGetCount) == 5; }, "Redo did not restore the created folder's contents");
    edit = start(root, true, L"cancelled"); send(edit, WM_KEYDOWN, VK_ESCAPE);
    check(!fs::exists(fixture.root / L"cancelled"), "Escape created a folder");
    start(root, false, L"focus-cancelled.txt"); click(tree, 30, 10);
    check(!FindWindowExW(tree, nullptr, treeInputClassName, nullptr) && !fs::exists(fixture.root / L"focus-cancelled.txt"), "Focus loss created a file");
    expand(tree, root, false);
    edit = start(root, false, L"root.txt"); send(edit, WM_KEYDOWN, VK_RETURN);
    waitFor([&] { return fs::exists(fixture.root / L"root.txt") && expanded(tree, root); }, "Cannot create inside a collapsed project root");
    fail(false, L"root.txt"); fail(true, L"root.txt"); fail(false, L"collapsed 日本語"); fail(true, L"collapsed 日本語");
    fail(false, L"../escape.txt"); fail(true, L"CON"); fail(false, L"");
    check(fs::file_size(fixture.root / L"root.txt") == 0 && fs::is_directory(created.parent_path()) && fs::exists(created), "Creation collision changed an existing item");
    check(visibleTabCount(app) == 2, "Failed or cancelled creation changed file tabs");
    keepTab(app, 1);
    const auto rootFile = nextItem(tree, TreeRelation::Next, folder);
    check(rootFile != 0, "Root file is missing before sibling creation");
    for (const auto target : {createdFile, rootFile}) {
        const auto parent = nextItem(tree, TreeRelation::Parent, target);
        const auto directory = target == createdFile ? created.parent_path() : fixture.root;
        edit = start(target, false, L"隣の ファイル.txt", parent); send(edit, WM_KEYDOWN, VK_RETURN);
        waitFor([&] { return fs::is_regular_file(directory / L"隣の ファイル.txt") && visibleTabCount(app) == 3; }, "File context menu did not create and open a sibling file");
        check(fs::file_size(directory / L"隣の ファイル.txt") == 0, "File context menu did not create an empty sibling file");
        waitFor([&] { return nextItem(tree, TreeRelation::Parent, static_cast<TreeItem>(send(tree, treeGetSelection))) == parent; }, "Sibling file was not selected in the same folder");
        edit = start(target, true, L"隣の フォルダー", parent); send(edit, WM_KEYDOWN, VK_RETURN);
        waitFor([&] { return fs::is_directory(directory / L"隣の フォルダー"); }, "File context menu did not create a sibling folder");
        waitFor([&] { return nextItem(tree, TreeRelation::Parent, static_cast<TreeItem>(send(tree, treeGetSelection))) == parent; }, "Sibling folder was not selected in the same folder");
        check(visibleTabCount(app) == 3, "Creating a sibling folder opened a file tab");
    }
}
void newFileShortcutTests(const fs::path& executable) {
    Fixture fixture; fs::create_directory(fixture.root / L"chosen 日本語");
    writeFile(fixture.root / L"chosen 日本語" / L"existing.txt");
    Application app(executable, fixture.root); const auto tree = app.tree();
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 2; }, "Shortcut fixture did not load");
    const auto root = nextItem(tree, TreeRelation::Root), folder = nextItem(tree, TreeRelation::Child, root);
    const auto start = [&](HWND target, const std::wstring& name) {
        const auto selected = static_cast<TreeItem>(send(tree, treeGetSelection));
        const auto anchor = selected ? selected : root;
        struct Input {
            DWORD thread; BYTE previous[256]{};
            explicit Input(DWORD target) : thread(target) {
                check(AttachThreadInput(GetCurrentThreadId(), thread, TRUE) != FALSE, "Cannot attach Ctrl+N input");
                GetKeyboardState(previous);
            }
            ~Input() { SetKeyboardState(previous); AttachThreadInput(GetCurrentThreadId(), thread, FALSE); }
        } input(app.process.dwThreadId);
        SetFocus(target);
        BYTE keys[256]{}; keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        check(SetKeyboardState(keys) && PostMessageW(target, WM_KEYDOWN, 'N', 0), "Cannot post Ctrl+N");
        HWND edit = nullptr;
        waitFor([&] { edit = FindWindowExW(tree, nullptr, treeInputClassName, nullptr); return edit != nullptr; }, "Ctrl+N did not show the path input");
        const auto value = bounds(tree, edit);
        const auto height = send(tree, treeGetRowHeight), anchorRow = send(tree, treeGetRow, anchor);
        const auto scroll = scrollInfo(tree);
        check(value.top == (anchorRow + 1 - scroll.nPos) * height && value.bottom == value.top + height,
              "Ctrl+N did not insert its input immediately below the selected item");
        int depth = 0;
        for (auto ancestor = anchor; ancestor; ancestor = nextItem(tree, TreeRelation::Parent, ancestor)) ++depth;
        if (!expanded(tree, anchor)) --depth;
        const auto dpi = GetDpiForWindow(tree);
        check(value.left == (depth + 1) * MulDiv(18, dpi, 96) + MulDiv(5, dpi, 96), "Ctrl+N input used the wrong indentation");
        check(IsWindowVisible(tree) && SetWindowTextW(edit, name.c_str()), "Ctrl+N did not reveal Explorer or accept the path");
        return edit;
    };
    const auto commit = [&](HWND edit) { check(PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 0), "Cannot commit new path"); };
    const auto selected = [&] { return static_cast<TreeItem>(send(tree, treeGetSelection)); };
    send(tree, treeSelect, folder);
    send(GetDlgItem(app.window, 3), BM_CLICK); // Invoke from the editor with Explorer hidden.
    auto edit = start(viewer, L"path/to/file.md"); commit(edit);
    const auto file = fixture.root / L"chosen 日本語" / L"path" / L"to" / L"file.md";
    waitFor([&] { return fs::is_regular_file(file) && send(tree, treeGetCount) == 6; }, "Ctrl+N did not create and reveal nested parents");
    check(fs::file_size(file) == 0 && visibleTabCount(app) == 1, "New file was not empty or did not open a tab");
    keepTab(app, 0);
    controlKey(app, tree, 'Z');
    waitFor([&] { return !fs::exists(fixture.root / L"chosen 日本語" / L"path") && send(tree, treeGetCount) == 3; }, "Undo did not remove automatically created intermediate folders");
    check(fs::exists(fixture.root / L"chosen 日本語" / L"existing.txt"), "Undo removed a preexisting parent or sibling");
    controlKey(app, tree, 'Z', true);
    waitFor([&] { return fs::is_regular_file(file) && send(tree, treeGetCount) == 6; }, "Ctrl+Shift+Z did not restore the nested creation");
    const auto to = nextItem(tree, TreeRelation::Parent, selected());
    check(to && nextItem(tree, TreeRelation::Child, to) == selected(), "Nested new file was not selected");
    // With a file selected, a trailing slash creates a sibling folder.
    edit = start(tree, L"nested/empty 日本語/"); commit(edit);
    waitFor([&] { return fs::is_directory(file.parent_path() / L"nested" / L"empty 日本語") && send(tree, treeGetCount) == 8; }, "Trailing slash did not create nested folders beside the selected file");
    check(visibleTabCount(app) == 1, "Trailing slash opened a file tab");
    // Reuse existing intermediate folders instead of treating them as a collision.
    send(tree, treeSelect, folder);
    edit = start(tree, L"path/to/second.txt"); commit(edit);
    waitFor([&] { return fs::is_regular_file(file.parent_path() / L"second.txt") && send(tree, treeGetCount) == 9; }, "Could not reuse existing intermediate folders");
    check(visibleTabCount(app) == 2, "Second new file did not open a tab");
    keepTab(app, 1);
    send(tree, treeSelect, root); expand(tree, root, false);
    edit = start(tree, L"cancelled/path.txt"); send(edit, WM_KEYDOWN, VK_ESCAPE);
    check(!fs::exists(fixture.root / L"cancelled") && !FindWindowExW(tree, nullptr, treeInputClassName, nullptr), "Escape created intermediate folders");
    edit = start(viewer, L"focus-cancelled/path.txt"); click(tree, 30, 10);
    check(!fs::exists(fixture.root / L"focus-cancelled") && !FindWindowExW(tree, nullptr, treeInputClassName, nullptr), "Focus loss created intermediate folders");
    // The full input, not only one 255-character component, reaches creation.
    const auto longPath = std::wstring(130, L'a') + L"/" + std::wstring(130, L'b') + L"/long.txt";
    edit = start(viewer, longPath);
    check(GetWindowTextLengthW(edit) == static_cast<int>(longPath.size()), "Ctrl+N truncated a path longer than 255 characters");
    send(edit, WM_KEYDOWN, VK_ESCAPE);
    // Keep the filesystem check below MAX_PATH so the test does not depend on
    // the host's long-path registry setting.
    edit = start(viewer, L"root.txt"); commit(edit);
    waitFor([&] { return fs::is_regular_file(fixture.root / L"root.txt") && expanded(tree, root); }, "Ctrl+N did not create a file inside the collapsed root");
    check(visibleTabCount(app) == 3, "Root file did not open a tab");
}
void pasteImageTests(const fs::path& executable) {
    Fixture fixture; fs::create_directory(fixture.root/L"images"); writeFile(fixture.root/L"images"/L"existing.txt");
    Application app(executable,fixture.root); const auto tree = app.tree();
    waitFor([&]{return send(tree,treeGetCount)==2;},"Image paste folder did not load");
    const auto root = nextItem(tree,TreeRelation::Root), folder = nextItem(tree,TreeRelation::Child,root);
    send(tree,treeSelect,folder);
    BITMAPINFOHEADER header{}; header.biSize = sizeof(header); header.biWidth = 1; header.biHeight = 1; header.biPlanes = 1; header.biBitCount = 24;
    const auto data = GlobalAlloc(GMEM_MOVEABLE,sizeof(header)+4); check(data != nullptr,"Cannot allocate clipboard image");
    const auto pixels = static_cast<char*>(GlobalLock(data)); check(pixels != nullptr,"Cannot lock clipboard image");
    memcpy(pixels,&header,sizeof(header)); memset(pixels+sizeof(header),0,4); pixels[sizeof(header)+2] = static_cast<char>(255); GlobalUnlock(data);
    check(OpenClipboard(app.host)!=FALSE,"Cannot open image test clipboard");
    EmptyClipboard(); const bool set = SetClipboardData(CF_DIB,data) != nullptr; CloseClipboard();
    if (!set) GlobalFree(data);
    check(set,"Cannot set image test clipboard");
    send(tree,WM_PASTE);
    fs::path created;
    waitFor([&]{for(const auto& entry:fs::directory_iterator(fixture.root/L"images")) if(entry.path().extension()==L".png") created=entry.path(); return !created.empty();},"Pasting into a collapsed folder did not create an image");
    const auto name = created.filename().wstring();
    check(name.size()==29 && name.substr(0,6)==L"paste-" && name[16]==L'T' && name.substr(25)==L".png","Image filename does not match the requested timestamp format");
    std::ifstream png(created,std::ios::binary); char signature[8]{}; png.read(signature,8);
    check(std::string(signature,8)==std::string("\x89PNG\r\n\x1a\n",8),"Explorer paste did not write a PNG");
    waitFor([&]{return send(tree,treeGetCount)==4 && send(tree,treeGetSelection)!=static_cast<LRESULT>(folder);},"Pasted image was not revealed and selected");
    png.close();
    controlKey(app, tree, 'Z');
    waitFor([&] { return !fs::exists(created) && send(tree, treeGetCount) == 3; }, "Undo did not remove the pasted image");
    controlKey(app, tree, 'Y');
    waitFor([&] { return fs::exists(created) && send(tree, treeGetCount) == 4; }, "Redo did not restore the pasted image");
    // A selected file uses its parent folder. Start in a later second so both
    // names follow the exact timestamp format without a collision suffix.
    const auto selected = send(tree,treeGetSelection);
    waitFor([&]{SYSTEMTIME now{}; GetLocalTime(&now); return std::stoi(name.substr(23,2)) != now.wSecond;},"Clock did not advance before the second paste");
    send(tree,WM_PASTE);
    waitFor([&]{return send(tree,treeGetCount)==5 && send(tree,treeGetSelection)!=selected;},"Pasting with a selected file did not use its parent folder");
    check(fs::exists(fixture.root/L"images"/L"existing.txt"),"Pasting removed another file");
}
void renameTests(const fs::path& executable) {
    Fixture fixture; writeLines(fixture.root/L"a.txt",120); writeLines(fixture.root/L"b.txt",240);
    {
        Application app(executable,fixture.root);const auto tree=app.tree();
        const auto viewer=FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr);
        waitFor([&]{return send(tree,treeGetCount)==3;},"Rename fixture did not load");
        const auto first=nextItem(tree,TreeRelation::Child,nextItem(tree,TreeRelation::Root));
        send(tree,treeSelect,first);waitLines(viewer,120);
        send(viewer,WM_CHAR,'\r');waitLines(viewer,121);
        send(tree,WM_KEYDOWN,VK_F2);const auto edit=FindWindowExW(tree,nullptr,treeInputClassName,nullptr);
        check(edit!=nullptr,"F2 did not open a filename editor");
        check(SetWindowTextW(edit,L"修正.py"),"Could not enter a Unicode filename");
        send(edit,WM_KEYDOWN,VK_RETURN);
        waitFor([&]{return fs::exists(fixture.root/L"修正.py")&&!fs::exists(fixture.root/L"a.txt");},"Rename did not move the file");
        controlKey(app, tree, 'Z');
        waitFor([&] { return fs::exists(fixture.root / L"a.txt") && !fs::exists(fixture.root / L"修正.py"); }, "Undo did not reverse a rename");
        waitLines(viewer, 121);
        controlKey(app, tree, 'Y');
        waitFor([&] { return fs::exists(fixture.root / L"修正.py") && !fs::exists(fixture.root / L"a.txt"); }, "Redo did not reapply a rename");
        waitLines(viewer,121);send(viewer,WM_KEYDOWN,VK_BACK);waitLines(viewer,120);
        check(visibleTabCount(app)==1,"Rename duplicated or closed the open tab");
        send(tree,WM_KEYDOWN,VK_F2);const auto cancel=FindWindowExW(tree,nullptr,treeInputClassName,nullptr);
        check(cancel!=nullptr,"Second rename editor was missing");SetWindowTextW(cancel,L"cancelled.txt");send(cancel,WM_KEYDOWN,VK_ESCAPE);
        check(!fs::exists(fixture.root/L"cancelled.txt")&&fs::exists(fixture.root/L"修正.py"),"Escape committed a rename");
        send(tree,WM_KEYDOWN,VK_F2);const auto collision=FindWindowExW(tree,nullptr,treeInputClassName,nullptr);SetWindowTextW(collision,L"b.txt");send(collision,WM_KEYDOWN,VK_RETURN);
        HWND dialog=nullptr; std::pair<DWORD,HWND*> dialogContext{app.process.dwProcessId,&dialog};
        waitFor([&]{EnumWindows([](HWND window,LPARAM data)->BOOL{auto& context=*reinterpret_cast<std::pair<DWORD,HWND*>*>(data);DWORD pid=0;GetWindowThreadProcessId(window,&pid);wchar_t name[32]{};GetClassNameW(window,name,32);if(pid==context.first&&std::wstring(name)==L"#32770"){*context.second=window;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&dialogContext));return dialog!=nullptr;},"Filename collision did not report an error");
        send(dialog,WM_COMMAND,IDOK);
        check(fs::exists(fixture.root/L"修正.py")&&fs::exists(fixture.root/L"b.txt"),"A filename collision overwrote a file");
    }
    {
        Application app(executable,fixture.root);app.tree();
        const auto viewer=FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr);waitLines(viewer,120);
        check(visibleTabCount(app)==1,"Restart lost the renamed tab");
    }
}
HWND explorerDeleteConfirmation(const Application& app) {
    HWND dialog = nullptr;
    waitFor([&] {
        dialog = FindWindowW(L"#32770", L"Poppy — Delete");
        DWORD pid = 0; if (dialog) GetWindowThreadProcessId(dialog, &pid);
        return dialog && pid == app.process.dwProcessId;
    }, "Nonempty folder deletion did not ask for confirmation");
    wchar_t cancel[32]{}, remove[32]{};
    GetDlgItemTextW(dialog, IDCANCEL, cancel, 32); GetDlgItemTextW(dialog, IDOK, remove, 32);
    check(std::wstring(cancel) == L"Cancel" && std::wstring(remove) == L"Delete", "Folder confirmation has incorrect button labels");
    check(send(dialog, DM_GETDEFID) == MAKELONG(IDCANCEL, DC_HASDEFID), "Folder deletion must default to Cancel");
    return dialog;
}
void explorerDeleteTests(const fs::path& executable) {
    Fixture fixture;
    const auto folder = fixture.root / L"a folder 日本語", single = fixture.root / L"b file 日本語.txt", keep = fixture.root / L"c keep.txt";
    fs::create_directories(folder / L"nested"); writeLines(folder / L"nested" / L"child.txt", 140);
    writeLines(single, 120); writeFile(keep);
    Application app(executable, fixture.root); const auto tree = app.tree();
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 4; }, "Delete fixture did not load");
    const auto root = nextItem(tree, TreeRelation::Root), directory = nextItem(tree, TreeRelation::Child, root);
    const auto file = nextItem(tree, TreeRelation::Next, directory);
    send(tree, treeSelect, root); send(tree, WM_KEYDOWN, VK_DELETE);
    check(fs::exists(fixture.root) && !send(tree, treeCanUndo), "Delete accepted the project root");
    send(tree, treeSelect, file); waitLines(viewer, 120);
    send(tree, WM_KEYDOWN, VK_F2);
    const auto input = FindWindowExW(tree, nullptr, treeInputClassName, nullptr);
    check(input && SetWindowTextW(input, L"name.txt"), "Delete test could not open the filename input");
    send(input, EM_SETSEL, 0, 1); send(input, WM_KEYDOWN, VK_DELETE);
    wchar_t name[32]{}; GetWindowTextW(input, name, 32);
    check(std::wstring(name) == L"ame.txt" && fs::exists(single), "Delete in a filename input deleted the resource");
    send(input, WM_KEYDOWN, VK_ESCAPE);
    send(viewer, WM_CHAR, '\r'); waitLines(viewer, 121);
    send(tree, WM_KEYDOWN, VK_DELETE, 1LL << 30);
    check(!send(tree, treeCanUndo) && fs::exists(single), "An auto-repeated Delete started another deletion");
    PostMessageW(tree, WM_KEYDOWN, VK_DELETE, 0);
    waitFor([&] { return !fs::exists(single) && send(tree, treeGetCount) == 3; }, "Delete did not remove the file without confirmation");
    check(visibleTabCount(app) == 1, "Deletion closed the open tab"); waitLines(viewer, 121);
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::exists(single) && send(tree, treeGetCount) == 4; }, "Undo did not restore the deleted file");
    waitLines(viewer, 121);
    controlKey(app, tree, 'Y');
    waitFor([&] { return !fs::exists(single) && send(tree, treeGetCount) == 3; }, "Redo did not delete the restored file");
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::exists(single) && send(tree, treeGetCount) == 4; }, "Second undo did not restore the file");
    waitLines(viewer, 121); send(viewer, WM_KEYDOWN, VK_BACK); waitLines(viewer, 120);
    // A collapsed folder must be checked on disk. Every cancellation path
    // leaves the resource and the existing Redo history intact.
    for (const auto cancel : {static_cast<int>(TDM_CLICK_BUTTON), WM_KEYDOWN, WM_CLOSE}) {
        send(tree, treeSelect, directory); PostMessageW(tree, WM_KEYDOWN, VK_DELETE, 0);
        const auto confirmation = explorerDeleteConfirmation(app);
        check(fs::exists(folder / L"nested" / L"child.txt") && fs::exists(single), "Folder deletion started before confirmation");
        if (cancel == TDM_CLICK_BUTTON) send(confirmation, cancel, IDCANCEL);
        else PostMessageW(confirmation, cancel, cancel == WM_KEYDOWN ? VK_ESCAPE : 0, 0);
        waitFor([&] { return !IsWindow(confirmation) && send(tree, treeCanRedo); }, "Cancellation changed Explorer history or left deletion pending");
        check(fs::exists(folder / L"nested" / L"child.txt") && send(tree, treeGetCount) == 4, "Cancellation deleted a collapsed folder");
    }
    expand(tree, directory);
    waitFor([&] { return nextItem(tree, TreeRelation::Child, directory) != 0; }, "Delete folder did not expand");
    expand(tree, nextItem(tree, TreeRelation::Child, directory));
    waitFor([&] { return send(tree, treeGetCount) == 6; }, "Delete subtree did not load");
    send(tree, treeSelect, directory); PostMessageW(tree, WM_KEYDOWN, VK_DELETE, 0);
    send(explorerDeleteConfirmation(app), TDM_CLICK_BUTTON, IDOK);
    waitFor([&] { return !fs::exists(folder) && send(tree, treeGetCount) == 3; }, "Delete did not remove a nonempty folder");
    check(fs::exists(single) && fs::exists(keep), "Folder deletion removed a sibling");
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::exists(folder / L"nested" / L"child.txt") && send(tree, treeGetCount) == 6; }, "Folder undo lost descendants or expansion state");
    controlKey(app, tree, 'Y');
    waitFor([&] { return !fs::exists(folder) && send(tree, treeGetCount) == 3; }, "Folder redo did not remove descendants");
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::exists(folder) && send(tree, treeGetCount) == 6; }, "Second folder undo failed");

    // A sharing violation reports an error and leaves the selected file intact.
    const auto locked = CreateFileW(keep.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "Cannot lock the deletion fixture");
    send(tree, treeSelect, nextItem(tree, TreeRelation::Next, nextItem(tree, TreeRelation::Next, nextItem(tree, TreeRelation::Child, root))));
    PostMessageW(tree, WM_KEYDOWN, VK_DELETE, 0);
    HWND dialog = nullptr;
    waitFor([&] { dialog = FindWindowW(L"#32770", L"Poppy — Delete"); return dialog != nullptr; }, "Failed deletion did not report an error");
    CloseHandle(locked); send(dialog, WM_COMMAND, IDOK);
    check(fs::exists(keep) && fs::exists(single) && fs::exists(folder), "Failed deletion changed a live resource");
}
void explorerDeleteConfirmationTests(const fs::path& executable) {
    Fixture fixture;
    const auto folder = fixture.root / L"a nonempty 日本語", empty = fixture.root / L"b empty", file = fixture.root / L"c.txt";
    fs::create_directory(folder); fs::create_directory(empty); writeFile(file);
    const auto hidden = folder / L".hidden"; writeFile(hidden);
    check(SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN) != FALSE, "Could not hide the deletion fixture");
    Application app(executable, fixture.root); const auto tree = app.tree();
    waitFor([&] { return send(tree, treeGetCount) == 4; }, "Batch confirmation fixture did not load");
    controlShortcut(app, tree, 'A', [&] { return send(tree, treeGetSelectionCount) == 3; }, "Could not select the deletion batch");
    PostMessageW(tree, WM_KEYDOWN, VK_DELETE, 0);
    auto dialog = explorerDeleteConfirmation(app);
    check(fs::exists(hidden) && fs::exists(empty) && fs::exists(file), "A mixed batch deleted items before confirmation");
    send(dialog, TDM_CLICK_BUTTON, IDCANCEL);
    waitFor([&] { return !IsWindow(dialog); }, "Batch cancellation did not close the confirmation");
    check(fs::exists(hidden) && fs::exists(empty) && fs::exists(file) && !send(tree, treeCanUndo), "Batch cancellation changed resources or Undo history");
    PostMessageW(tree, WM_KEYDOWN, VK_DELETE, 0);
    dialog = explorerDeleteConfirmation(app);
    // UI completions may change the selection during a modal dialog. Deletion
    // must still use the complete batch that the user was asked to confirm.
    const auto root = nextItem(tree, TreeRelation::Root);
    const auto emptyItem = nextItem(tree, TreeRelation::Next, nextItem(tree, TreeRelation::Child, root));
    send(tree, treeSelect, emptyItem);
    send(dialog, TDM_CLICK_BUTTON, IDOK);
    waitFor([&] { return !fs::exists(folder) && !fs::exists(empty) && !fs::exists(file) && send(tree, treeGetCount) == 1; }, "Confirmation did not delete the original batch");
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::exists(hidden) && fs::exists(empty) && fs::exists(file) && send(tree, treeGetCount) == 4; }, "Undo did not restore the confirmed batch");
    const auto restoredEmpty = nextItem(tree, TreeRelation::Next, nextItem(tree, TreeRelation::Child, root));
    send(tree, treeSelect, restoredEmpty); PostMessageW(tree, WM_KEYDOWN, VK_DELETE, 0);
    waitFor([&] { return !fs::exists(empty) && send(tree, treeGetCount) == 3; }, "Empty folder deletion unnecessarily asked for confirmation");
    check(fs::exists(hidden) && fs::exists(file), "Empty folder deletion removed another selected item");
}
void explorerMultipleSelectionTests(const fs::path& executable) {
    Fixture fixture;
    const auto target = fixture.root / L"a target", source = fixture.root / L"b folder";
    fs::create_directory(target); fs::create_directory(source);
    writeLines(source / L"child.txt", 200);
    const auto c = fixture.root / L"c.txt", d = fixture.root / L"d.txt", e = fixture.root / L"e.txt";
    writeLines(c, 120); writeLines(d, 140); writeLines(e, 160);
    Application app(executable, fixture.root); const auto tree = app.tree();
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 6; }, "Multiple selection fixture did not load");
    const auto root = nextItem(tree, TreeRelation::Root);
    const auto itemAt = [&](int index) {
        auto item = nextItem(tree, TreeRelation::Child, root);
        while (index-- && item) item = nextItem(tree, TreeRelation::Next, item);
        return item;
    };
    const auto rowClick = [&](TreeItem item, WPARAM modifiers = 0) {
        const int height = static_cast<int>(send(tree, treeGetRowHeight));
        const auto x = MulDiv(100, GetDpiForWindow(tree), 96);
        const auto y = (static_cast<int>(send(tree, treeGetRow, item)) - scrollInfo(tree).nPos) * height + height / 2;
        send(tree, WM_LBUTTONDOWN, MK_LBUTTON | modifiers, MAKELPARAM(x, y));
        send(tree, WM_LBUTTONUP, modifiers, MAKELPARAM(x, y));
    };
    const auto shortcut = [&](WPARAM key) {
        check(AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, TRUE) != FALSE, "Cannot attach Explorer clipboard input");
        BYTE previous[256]{}, keys[256]{}; GetKeyboardState(previous);
        keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        SetKeyboardState(keys); send(tree, WM_KEYDOWN, key); SetKeyboardState(previous);
        AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, FALSE);
    };
    send(tree, treeSelect, itemAt(2)); waitLines(viewer, 120);
    rowClick(itemAt(4), MK_SHIFT);
    check(send(tree, treeGetSelectionCount) == 3 && send(tree, treeIsSelected, itemAt(3)) && lastTextRow(viewer) == 120,
        "Shift click did not select the range or opened its last file");
    rowClick(itemAt(3), MK_CONTROL);
    check(send(tree, treeGetSelectionCount) == 2 && !send(tree, treeIsSelected, itemAt(3)), "Ctrl click did not remove a row");
    rowClick(itemAt(3), MK_CONTROL);
    check(send(tree, treeGetSelectionCount) == 3, "Ctrl click did not add a row");
    send(tree, WM_KEYDOWN, VK_F2);
    check(!FindWindowExW(tree, nullptr, treeInputClassName, nullptr), "Multiple selection renamed only one of its files");
    PostMessageW(tree, WM_KEYDOWN, VK_DELETE, 0);
    waitFor([&] { return !fs::exists(c) && !fs::exists(d) && !fs::exists(e) && send(tree, treeGetCount) == 3; }, "Delete did not remove the entire selection");
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::exists(c) && fs::exists(d) && fs::exists(e) && send(tree, treeGetSelectionCount) == 3; }, "One undo did not restore and select all deleted files");
    controlKey(app, tree, 'Y');
    waitFor([&] { return !fs::exists(c) && !fs::exists(d) && !fs::exists(e); }, "One redo did not delete all files");
    controlKey(app, tree, 'Z');
    waitFor([&] { return send(tree, treeGetCount) == 6 && send(tree, treeGetSelectionCount) == 3; }, "Batch delete could not be undone again");
    shortcut('C'); check(IsClipboardFormatAvailable(CF_HDROP), "Explorer copy did not publish Windows file clipboard data");
    send(tree, treeSelect, itemAt(0)); shortcut('V');
    waitFor([&] { return fs::exists(target / L"c.txt") && fs::exists(target / L"d.txt") && fs::exists(target / L"e.txt") && send(tree, treeGetSelectionCount) == 3; }, "Paste did not copy and select all files");
    check(fs::exists(c) && fs::exists(d) && fs::exists(e), "Copy removed its sources");
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::is_empty(target); }, "One undo did not remove the whole pasted batch");
    controlKey(app, tree, 'Y');
    waitFor([&] { return fs::exists(target / L"c.txt") && fs::exists(target / L"d.txt") && fs::exists(target / L"e.txt"); }, "One redo did not restore the pasted batch");
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::is_empty(target) && send(tree, treeGetCount) == 6; }, "Pasted files were not removed before the next test");
    send(tree, treeSelect, itemAt(2)); shortcut('C'); shortcut('V');
    waitFor([&] { return fs::exists(fixture.root / L"c - Copy.txt") && send(tree, treeGetCount) == 7; }, "Copy in the source folder did not create a sibling with its extension");
    controlKey(app, tree, 'Z');
    waitFor([&] { return !fs::exists(fixture.root / L"c - Copy.txt") && send(tree, treeGetCount) == 6; }, "Undo did not remove the duplicate");
    const auto folder = itemAt(1); expand(tree, folder);
    waitFor([&] { return nextItem(tree, TreeRelation::Child, folder) != 0; }, "Copy source folder did not load");
    send(tree, treeSelect, folder); rowClick(nextItem(tree, TreeRelation::Child, folder), MK_CONTROL); rowClick(itemAt(2), MK_CONTROL);
    check(send(tree, treeGetSelectionCount) == 3, "Folder and child could not be selected together");
    shortcut('X'); send(tree, treeSelect, itemAt(0)); shortcut('V');
    waitFor([&] { return !fs::exists(source) && !fs::exists(c) && fs::exists(target / L"b folder" / L"child.txt") && fs::exists(target / L"c.txt"); }, "Cut did not move a folder and file or processed the child twice");
    controlKey(app, tree, 'Z');
    waitFor([&] { return fs::exists(source / L"child.txt") && fs::exists(c) && fs::is_empty(target); }, "One undo did not restore the cut batch");
    shortcut('A');
    check(!send(tree, treeIsSelected, root) && send(tree, treeGetSelectionCount) >= 5, "Select all included the protected project root");
}
void copyFolderAcrossProjects(const fs::path& executable, bool copyRoot) {
    Fixture a, b;
    const auto source = a.root / L"folder 日本語";
    const auto copied = copyRoot ? a.root : source, destination = b.root / copied.filename();
    const auto destinationFolder = copyRoot ? destination / source.filename() : destination;
    fs::create_directories(source / L"nested" / L"empty");
    writeLines(source / L"nested" / L"data.txt", 3);
    Application app(executable, a.root); const auto treeA = app.tree();
    waitFor([&] { return send(treeA, treeGetCount) == 2; }, "Folder copy source project did not load");
    const auto rootA = nextItem(treeA, TreeRelation::Root);
    send(treeA, treeSelect, copyRoot ? rootA : nextItem(treeA, TreeRelation::Child, rootA));
    const auto sequence = GetClipboardSequenceNumber();
    if (copyRoot) {
        send(treeA, WM_CUT);
        check(GetClipboardSequenceNumber() == sequence && fs::exists(source), "The project root was cut");
        send(treeA, WM_COPY);
        check(GetClipboardSequenceNumber() != sequence, "The project root could not be copied");
    } else controlShortcut(app, treeA, 'C', [&] { return GetClipboardSequenceNumber() != sequence; }, "Could not copy a project folder");
    check(IsClipboardFormatAvailable(CF_HDROP), "Folder copy did not publish file clipboard data");
    const auto text = b.root.native();
    COPYDATASTRUCT data{0x50505901, static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(text.c_str())};
    check(send(app.host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)), "Could not open the folder copy destination project");
    const auto treeB = app.tree();
    check(treeA != treeB, "Folder copy did not switch projects");
    const auto rootB = nextItem(treeB, TreeRelation::Root);
    send(treeB, treeSelect, rootB);
    controlShortcut(app, treeB, 'V', [&] {
        return fs::exists(destinationFolder / L"nested" / L"data.txt") && send(treeB, treeCanUndo) != 0;
    }, "Cross-project folder paste did not finish");
    const auto checkContents = [&] {
        check(readText(destinationFolder / L"nested" / L"data.txt") == readText(source / L"nested" / L"data.txt") &&
            fs::is_empty(destinationFolder / L"nested" / L"empty"), "Cross-project folder copy lost contents or an empty directory");
    };
    checkContents();
    waitFor([&] {
        const auto selection = static_cast<TreeItem>(send(treeB, treeGetSelection));
        return selection && selection != rootB;
    }, "Copied folder was not selected");
    check(send(treeA, treeCanUndo) == 0, "Copy changed the source project's history");
    controlKey(app, treeB, 'Z');
    waitFor([&] { return !fs::exists(destination); }, "Undo did not remove the copied folder");
    check(fs::exists(source / L"nested" / L"data.txt"), "Undo of a folder copy changed its source project");
    controlKey(app, treeB, 'Y');
    waitFor([&] { return fs::exists(destinationFolder / L"nested" / L"data.txt"); }, "Redo did not restore the copied folder");
    checkContents();
}
void crossProjectFolderCopyTests(const fs::path& executable) {
    copyFolderAcrossProjects(executable, false);
    copyFolderAcrossProjects(executable, true);
}
void explorerMoveTests(const fs::path& executable) {
    Fixture fixture;
    const auto source = fixture.root / L"a source", target = fixture.root / L"b target 日本語";
    fs::create_directories(source / L"nested"); fs::create_directory(target);
    writeLines(source / L"nested" / L"edited.txt", 120);
    writeLines(fixture.root / L"collision.txt", 140); writeLines(target / L"collision.txt", 240);
    const auto movedFile = target / L"nested" / L"edited.txt";
    {
        Application app(executable, fixture.root); const auto tree = app.tree();
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitFor([&] { return send(tree, treeGetCount) == 4; }, "Move fixture did not load");
        const auto root = nextItem(tree, TreeRelation::Root), from = nextItem(tree, TreeRelation::Child, root);
        const auto to = nextItem(tree, TreeRelation::Next, from), collision = nextItem(tree, TreeRelation::Next, to);
        expand(tree, from);
        waitFor([&] { return nextItem(tree, TreeRelation::Child, from) != 0; }, "Move source did not expand");
        const auto nested = nextItem(tree, TreeRelation::Child, from); expand(tree, nested);
        waitFor([&] { return nextItem(tree, TreeRelation::Child, nested) != 0; }, "Nested move source did not expand");
        const auto file = nextItem(tree, TreeRelation::Child, nested);
        const auto point = [&](TreeItem id, int x = 110) {
            const int height = static_cast<int>(send(tree, treeGetRowHeight));
            return POINT{MulDiv(x, GetDpiForWindow(tree), 96),
                (static_cast<int>(send(tree, treeGetRow, id)) - scrollInfo(tree).nPos) * height + height / 2};
        };
        const auto drag = [&](TreeItem id, POINT destination, UINT cancel = 0) {
            const auto origin = point(id);
            send(tree, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(origin.x, origin.y));
            send(tree, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(destination.x, destination.y));
            if (cancel) send(tree, cancel, cancel == WM_KEYDOWN ? VK_ESCAPE : 0);
            send(tree, WM_LBUTTONUP, 0, MAKELPARAM(destination.x, destination.y));
        };
        const auto origin = point(file); click(tree, origin.x, origin.y); waitLines(viewer, 120);
        check(fs::exists(source / L"nested" / L"edited.txt"), "A click moved a file");
        send(viewer, WM_CHAR, '\r'); waitLines(viewer, 121);
        const auto hoverOrigin = point(nested), hoverTarget = point(to);
        send(tree, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(hoverOrigin.x, hoverOrigin.y));
        send(tree, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(hoverTarget.x, hoverTarget.y));
        waitFor([&] { return expanded(tree, to); }, "Hovering during a drag did not expand the destination");
        send(tree, WM_KEYDOWN, VK_ESCAPE);
        send(tree, WM_LBUTTONUP, 0, MAKELPARAM(hoverTarget.x, hoverTarget.y));
        for (const UINT cancel : {WM_KEYDOWN, WM_CANCELMODE, WM_CAPTURECHANGED}) {
            drag(nested, point(to), cancel);
            check(fs::exists(source / L"nested") && !fs::exists(target / L"nested"), "Cancelled drag moved a folder");
            check(expanded(tree, nested), "Cancelled folder drag toggled the source folder");
        }
        drag(nested, {-20, -20});
        drag(from, point(nested)); // Cannot move a folder into its descendants.
        drag(nested, point(nested, 160)); // Cannot move into itself.
        drag(nested, point(from)); // Already in this folder.
        drag(root, point(to)); // The project root cannot be moved.
        check(expanded(tree, root), "Dragging the project root collapsed its children");
        check(fs::exists(source / L"nested" / L"edited.txt") && !fs::exists(target / L"nested"), "Invalid drag moved a folder");
        drag(nested, point(to));
        waitFor([&] { return fs::exists(movedFile) && !fs::exists(source / L"nested"); }, "Dragging a folder did not move its descendants");
        waitFor([&] { return expanded(tree, to) && nextItem(tree, TreeRelation::Parent, nested) == to && expanded(tree, nested); },
            "Moving did not preserve the subtree or expand its destination");
        waitLines(viewer, 121);
        check(visibleTabCount(app) == 1, "Moving a folder closed or duplicated its edited tab");
        controlKey(app, tree, 'Z');
        waitFor([&] { return fs::exists(source / L"nested" / L"edited.txt") && nextItem(tree, TreeRelation::Parent, nested) == from; }, "Undo did not reverse a folder move in the filesystem and tree");
        waitLines(viewer, 121);
        controlKey(app, tree, 'Z', true);
        waitFor([&] { return fs::exists(movedFile) && nextItem(tree, TreeRelation::Parent, nested) == to; }, "Redo did not restore the folder move");
        waitLines(viewer, 121);
        {
            check(AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, TRUE) != FALSE, "Cannot attach save input");
            BYTE previous[256]{}, keys[256]{}; GetKeyboardState(previous); keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
            SetKeyboardState(keys); send(viewer, WM_KEYDOWN, 'S'); SetKeyboardState(previous);
            AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, FALSE);
        }
        waitFor([&] { std::ifstream saved(movedFile); const auto first = saved.peek(); return first == '\n' || first == '\r'; }, "Saving an edited moved tab used its old path");
        check(!fs::exists(source / L"nested" / L"edited.txt"), "Saving recreated the old path");
        drag(collision, point(to));
        HWND dialog = nullptr;
        waitFor([&] {
            dialog = FindWindowW(L"#32770", L"Poppy — Move"); DWORD pid = 0; if (dialog) GetWindowThreadProcessId(dialog, &pid);
            return dialog && pid == app.process.dwProcessId;
        }, "Move collision did not report an error");
        send(dialog, WM_COMMAND, IDOK);
        check(fs::file_size(fixture.root / L"collision.txt") != fs::file_size(target / L"collision.txt"), "Move collision overwrote a file");
        // Moving an individual file to the root also relocates its existing tab.
        const int height = static_cast<int>(send(tree, treeGetRowHeight));
        const POINT blank{point(root).x, static_cast<LONG>((send(tree, treeGetCount) - scrollInfo(tree).nPos) * height + height / 2)};
        drag(file, blank);
        waitFor([&] { return fs::exists(fixture.root / L"edited.txt") && !fs::exists(movedFile); }, "Cannot drag a file back to the project root");
        waitFor([&] { return nextItem(tree, TreeRelation::Parent, file) == root; }, "File move left the tree under the old parent");
        waitLines(viewer, 121); check(visibleTabCount(app) == 1, "File move duplicated its tab");
        controlKey(app, tree, 'Z');
        waitFor([&] { return fs::exists(movedFile) && nextItem(tree, TreeRelation::Parent, file) == nested; }, "Undo did not reverse a file move");
        controlKey(app, tree, 'Y');
        waitFor([&] { return fs::exists(fixture.root / L"edited.txt") && nextItem(tree, TreeRelation::Parent, file) == root; }, "Redo did not restore the file move");
        drag(nested, point(root));
        waitFor([&] { return fs::is_directory(fixture.root / L"nested") && !fs::exists(target / L"nested"); }, "Cannot drop a folder onto the root row");
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitLines(viewer, 121); check(visibleTabCount(app) == 1, "Restart did not restore the moved tab");
    }
}
void splitPaneTests(const fs::path& executable) {
    Fixture fixture; writeLines(fixture.root / L"a.txt", 120); writeLines(fixture.root / L"b.txt", 240);
    LONG divider = 0;
    {
        Application app(executable, fixture.root); const auto tree = app.tree();
        const auto left = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        HWND right = nullptr;
        waitFor([&] { return send(tree, treeGetCount) == 3; }, "Split fixture did not load");
        const auto first = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        send(tree, treeSelect, first); waitLines(left, 120); keepTab(app, 0);
        send(tree, treeSelect, nextItem(tree, TreeRelation::Next, first)); waitLines(left, 240);
        const auto original = bounds(app.window, left);
        dragTab(app, 1, {original.right - 10, (original.top + original.bottom) / 2});
        right = otherVisibleViewer(app.window, left);
        waitFor([&] { return IsWindowVisible(right); }, "Editor drag did not create a right group");
        waitLines(left, 120); waitLines(right, 240);
        const auto l = bounds(app.window,left), r = bounds(app.window,right);
        check(l.right < r.left && l.bottom == r.bottom, "Editors overlap or do not share a bottom edge");
        click(left, 100, 40);
        check(send(tree, treeGetSelection) == static_cast<LRESULT>(first), "Explorer did not follow focus to the left editor");
        click(right, 100, 40);
        check(send(tree, treeGetSelection) == static_cast<LRESULT>(nextItem(tree, TreeRelation::Next, first)), "Explorer did not follow focus to the right editor");
        send(right, WM_CHAR, '\r'); waitLines(right, 241);
        clickTab(app, 0); waitLines(left, 120); clickTab(app, 1); waitLines(right, 241);
        send(right, WM_KEYDOWN, VK_BACK); waitLines(right, 240);
        const int x = (l.right+r.left)/2, y = l.top+40;
        send(app.window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        send(app.window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+30,y));
        send(app.window,WM_LBUTTONUP,0,MAKELPARAM(x+30,y)); divider=bounds(app.window,left).right;
        check(divider>l.right,"Editor divider did not move");
        const auto terminalLeft = FindWindowExW(app.window, nullptr, L"PoppyTerminal", nullptr);
        addTerminal(app, terminalLeft);
        const auto terminalRight = FindWindowExW(app.window, terminalLeft, L"PoppyTerminal", nullptr);
        const auto body = bounds(app.window, terminalRight);
        dragTerminalTab(app, terminalRight, {body.right - 10, (body.top + body.bottom) / 2});
        check(IsWindowVisible(terminalLeft) && IsWindowVisible(terminalRight), "Terminal drag did not show two sessions");
        check(bounds(app.window, terminalLeft).right < bounds(app.window, terminalRight).left, "Terminal sessions overlap");
    }
    {
        Application app(executable,fixture.root);app.tree();
        const auto left=FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr),right=otherVisibleViewer(app.window,left);
        waitLines(left,120);waitLines(right,240);
        check(IsWindowVisible(right)&&bounds(app.window,left).right==divider,"Restart lost editor groups or divider");
        const auto pane=app.window;
        const auto a=FindWindowExW(pane,nullptr,L"PoppyTerminal",nullptr),b=FindWindowExW(pane,a,L"PoppyTerminal",nullptr);
        check(IsWindowVisible(a)&&IsWindowVisible(b),"Restart lost terminal groups");
        const auto target=tabBounds(app,0);dragTab(app,1,{target.right-1,(target.top+target.bottom)/2});
        waitFor([&]{return !IsWindowVisible(right);},"Moving the last right tab did not collapse the split");
        waitLines(left,240);
    }
}
std::set<DWORD> childProcesses(DWORD parent);
void dockLayoutTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 5; ++i) writeLines(fixture.root / (std::wstring(1, L'a' + i) + L".txt"), 100 + i * 10);
    std::vector<RECT> savedBounds;
    {
        Application app(executable, fixture.root); const auto tree = app.tree();
        const auto original = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitFor([&] { return send(tree, treeGetCount) == 6; }, "Dock fixture did not load");
        auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        for (int i = 0; i < 5; ++i) {
            send(tree, treeSelect, item); waitLines(original, 100 + i * 10); keepTab(app, i);
            item = nextItem(tree, TreeRelation::Next, item);
        }
        tabDragPreview(app, 0, original);
        for (int i = 0; i < 5; ++i) { clickTab(app, i); waitLines(original, 100 + i * 10); }
        clickTab(app, 0); send(original, WM_CHAR, '!');
        const auto shell = FindWindowExW(app.window, nullptr, L"PoppyTerminal", nullptr);
        click(shell, 10, 10);
        for (const auto ch : std::wstring(L"[Console]::Title = 'Dock session'\r")) send(shell, WM_CHAR, ch);
        waitFor([&] { wchar_t text[128]{}; GetWindowTextW(shell, text, 128); return std::wstring(text) == L"Dock session"; }, "Dock shell did not start");
        const auto topTab = tabBounds(app, 0);
        for (const auto cancel : {WM_CANCELMODE, WM_KEYDOWN}) {
            dragTerminalTab(app, shell, {topTab.right - 2, (topTab.top + topTab.bottom) / 2}, cancel);
            check(IsWindowVisible(original) && IsWindowVisible(shell), "Cancelling a dock drag changed the layout");
        }
        dragTerminalTab(app, shell, {topTab.right - 2, (topTab.top + topTab.bottom) / 2});
        check(!IsWindowVisible(original) && IsWindowVisible(shell), "Terminal did not join the editor's tab strip");
        clickTab(app, 0); waitLines(original, 100);
        clickTerminalTab(app, shell);
        const auto whole = bounds(app.window, shell);
        dragTab(app, 0, {(whole.left + whole.right) / 2, whole.bottom - 10});
        const auto viewerFor = [&](int lines) {
            HWND result = nullptr;
            waitFor([&] {
                for (HWND view = nullptr; (view = FindWindowExW(app.window, view, L"PoppyViewer", nullptr));)
                    if (IsWindowVisible(view) && lastTextRow(view) == lines) { result = view; return true; }
                return false;
            }, "Docked editor did not show its original document");
            return result;
        };
        const auto lower = viewerFor(100);
        check(bounds(app.window, shell).bottom < bounds(app.window, lower).top, "Editor did not move below the terminal");
        tabDragPreview(app, visibleTabCount(app) - 1, lower, false);
        controlShortcut(app, lower, 'J', [&] { return !IsWindowVisible(lower); }, "Ctrl+J from an editor did not hide the lower editor");
        check(IsWindowVisible(shell), "Ctrl+J hid a terminal in the upper region");
        controlShortcut(app, shell, VK_OEM_3, [&] { return IsWindowVisible(lower) != FALSE; }, "Ctrl+` from a terminal did not restore the lower editor");
        const auto save = [&] {
            std::ifstream file(fixture.root / L"a.txt"); char first = 0; file.get(first); return first == '!';
        };
        controlShortcut(app, lower, 'S', save, "Dock movement lost an unsaved editor buffer");
        HWND destination = lower;
        for (int i = 1; i < 5; ++i) {
            const auto rect = bounds(app.window, destination);
            POINT target = i == 1 ? POINT{rect.right - 10, (rect.top + rect.bottom) / 2} :
                i == 2 ? POINT{(rect.left + rect.right) / 2, rect.bottom - 8} :
                i == 3 ? POINT{rect.left + 8, (rect.top + rect.bottom) / 2} : POINT{(rect.left + rect.right) / 2, rect.top + 1};
            dragTab(app, 0, target); destination = viewerFor(100 + i * 10);
        }
        int visible = 0;
        for (HWND view = nullptr; (view = FindWindowExW(app.window, view, L"PoppyViewer", nullptr));) if (IsWindowVisible(view)) ++visible;
        check(visible == 5 && IsWindowVisible(shell) && visibleTabCount(app) == 5, "Dock layout is limited to four regions");
        // The same running ConPTY session must survive every reparent/layout operation.
        for (const auto ch : std::wstring(L"Set-Content -NoNewline -Encoding Ascii dock-alive.txt alive\r")) send(shell, WM_CHAR, ch);
        waitFor([&] { std::ifstream file(fixture.root / L"dock-alive.txt"); std::string text; return file && std::getline(file, text) && text == "alive"; }, "Dock movement restarted or stopped the terminal");
        for (int i = 0; i < 5; ++i) savedBounds.push_back(bounds(app.window, viewerFor(100 + i * 10)));
    }
    {
        Application app(executable, fixture.root); app.tree();
        for (int i = 0; i < 5; ++i) {
            HWND found = nullptr;
            waitFor([&] {
                for (HWND view = nullptr; (view = FindWindowExW(app.window, view, L"PoppyViewer", nullptr));)
                    if (IsWindowVisible(view) && lastTextRow(view) == 100 + i * 10) { found = view; return true; }
                return false;
            }, "Restart lost a recursively docked editor");
            const auto rect = bounds(app.window, found);
            check(EqualRect(&rect, &savedBounds[i]), "Restart changed a nested dock layout");
        }
        const auto shell = FindWindowExW(app.window, nullptr, L"PoppyTerminal", nullptr);
        check(IsWindowVisible(shell) && bounds(app.window, shell).top < savedBounds.front().top, "Restart lost the upper terminal placement");
        check(childProcesses(app.process.dwProcessId).empty(), "Restoring a dock layout automatically started a terminal");
    }
}
void sidebarTests(const fs::path& executable) {
    Fixture fixture;
    const auto folder = fixture.root / L"nested";
    fs::create_directory(folder); writeFile(folder / L"file.txt");
    Application app(executable, fixture.root);
    const auto tree = app.tree();
    const auto explorer = FindWindowExW(app.window, nullptr, activityButtonClassName, L"Explorer");
    const auto git = GetDlgItem(app.window, 3);
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    const auto terminal = FindWindowExW(app.window, nullptr, L"PoppyTerminal", nullptr);
    check(explorer && git && viewer && terminal, "Activity buttons or panes are missing");
    TreeItem nested = 0;
    waitFor([&] {
        nested = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        return nested != 0;
    }, "Explorer did not load its folder");
    expand(tree, nested);
    waitFor([&] { return expanded(tree, nested) && nextItem(tree, TreeRelation::Child, nested); }, "Explorer did not expand its folder");
    const UINT dpi = GetDpiForWindow(app.window);
    check(SetWindowPos(app.window, nullptr, 0, 0, MulDiv(1180, dpi, 96), MulDiv(820, dpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not size the test window for its DPI");
    const auto activityRect = bounds(app.window, explorer), treeRect = bounds(app.window, tree), viewerRect = bounds(app.window, viewer);
    if constexpr (sidebarOnRight) {
        RECT client{}; GetClientRect(app.window, &client);
        check(viewerRect.left == 0 && viewerRect.right < treeRect.left && treeRect.right < activityRect.left && activityRect.right == client.right,
              "Editor, sidebar and activity bar are not arranged from left to right");
    } else {
        check(activityRect.left == 0 && activityRect.right <= treeRect.left && treeRect.right < viewerRect.left,
              "Activity bar, sidebar and editor are not arranged from left to right");
    }
    check(activityRect.top == 0 && viewerRect.top == MulDiv(34, dpi, 96), "The removed toolbar still reserves space above the workspace");
    check(treeRect.top == viewerRect.top, "Explorer and empty dock headers are misaligned");
    send(git, WM_KEYDOWN, VK_SPACE);
    check(IsWindowVisible(tree), "Space activated the activity button before key release");
    send(git, WM_KEYUP, VK_SPACE);
    check(!IsWindowVisible(tree), "Git activity did not hide Explorer");
    const auto gitViewer = bounds(app.window, viewer);
    check(gitViewer.left == viewerRect.left && gitViewer.top == viewerRect.top,
          "Switching sidebars moved the editor");
    // Refreshing the hidden custom control must not change its visibility.
    writeFile(fixture.root / L"added.txt");
    check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh the hidden Explorer");
    waitFor([&] { return send(tree, treeGetCount) == 4; }, "Hidden Explorer did not refresh");
    check(!IsWindowVisible(tree), "Refreshing Explorer covered the Git sidebar");
    send(explorer, WM_KEYDOWN, VK_RETURN);
    check(IsWindowVisible(tree) && expanded(tree, nested), "Switching back lost Explorer's expansion state");
    send(git, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    send(git, WM_CANCELMODE);
    send(git, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
    check(IsWindowVisible(tree), "A cancelled activity-button press changed the sidebar");
    send(git, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    send(git, WM_LBUTTONUP, 0, MAKELPARAM(-10, -10));
    check(IsWindowVisible(tree), "Releasing outside an activity button changed the sidebar");
    click(git, 10, 10);
    check(!IsWindowVisible(tree), "Clicking the custom activity button did not switch sidebars");
    send(explorer, BM_CLICK);

    const int delta = MulDiv(48, dpi, 96);
    const int x = sidebarOnRight ? (viewerRect.right + treeRect.left) / 2 : (treeRect.right + viewerRect.left) / 2;
    const int y = treeRect.top + 20;
    send(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    // Keep the button held while reversing direction, as in a live resize.
    for (const int offset : {delta, 2 * delta, -delta, 0, delta}) {
        send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + offset, y));
        const auto resizedTree = bounds(app.window, tree), resizedViewer = bounds(app.window, viewer);
        const auto resizedTerminal = bounds(app.window, terminal);
        const int edge = sidebarOnRight ? resizedTree.left : resizedTree.right;
        const int originalEdge = sidebarOnRight ? treeRect.left : treeRect.right;
        check(edge >= originalEdge + offset - 1 && edge <= originalEdge + offset + 1,
              "Dragging the sidebar boundary did not resize the sidebar");
        check(sidebarOnRight
                  ? resizedTree.left - resizedViewer.right == treeRect.left - viewerRect.right && resizedTree.right == treeRect.right && resizedViewer.left == viewerRect.left
                  : resizedViewer.left - resizedTree.right == viewerRect.left - treeRect.right && resizedTree.left == treeRect.left && resizedViewer.right == viewerRect.right,
              "Resizing moved the activity bar or overlapped the editor");
        auto resizedActivity = bounds(app.window, explorer);
        check(EqualRect(&resizedActivity, &activityRect) != FALSE, "Resizing moved the activity bar");
        check(resizedTerminal.left == resizedViewer.left && resizedTerminal.right == resizedViewer.right,
              "Dragging the sidebar boundary did not align the terminal with the editor");
    }
    send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(x + delta, y));

    const auto editor = bounds(app.window, viewer), console = bounds(app.window, terminal);
    const int terminalX = editor.left + delta, terminalY = editor.bottom;
    send(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(terminalX, terminalY));
    for (const int offset : {-delta, delta, -2 * delta, 0}) {
        send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(terminalX, terminalY + offset));
        const auto resizedViewer = bounds(app.window, viewer), resizedTerminal = bounds(app.window, terminal);
        check(resizedTerminal.top >= console.top + offset - 1 && resizedTerminal.top <= console.top + offset + 1,
              "Dragging the terminal boundary did not change its height");
        check(resizedTerminal.top - resizedViewer.bottom == console.top - editor.bottom && resizedTerminal.bottom == console.bottom,
              "Dragging the terminal boundary overlapped the editor or moved the footer");
    }
    send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(terminalX, terminalY));
}
void treeInteractionTests(const fs::path& executable) {
    Fixture fixture;
    const auto folder = fixture.root / L"000 folder";
    fs::create_directory(folder); writeFile(folder / L"nested.txt");
    for (int i = 0; i < 120; ++i) writeFile(fixture.root / (L"file-" + std::to_wstring(1000 + i) + L".txt"));
    writeFile(fixture.root / (std::wstring(100, L'x') + L".txt"));
    Application app(executable, fixture.root);
    const auto tree = app.tree();
    waitFor([&] { return send(tree, treeGetCount) == 123; }, "The large tree did not load");
    const auto root = nextItem(tree, TreeRelation::Root), directory = nextItem(tree, TreeRelation::Child, root);
    const auto firstFile = nextItem(tree, TreeRelation::Next, directory);
    check(root && directory && firstFile, "Tree interaction fixtures are missing");
    send(tree, treeSelect, directory); send(tree, WM_KEYDOWN, VK_RIGHT);
    waitFor([&] { return nextItem(tree, TreeRelation::Child, directory) != 0; }, "Right arrow did not expand the custom tree");
    const auto nested = nextItem(tree, TreeRelation::Child, directory);
    send(tree, WM_KEYDOWN, VK_RIGHT);
    check(static_cast<TreeItem>(send(tree, treeGetSelection)) == nested, "Right arrow did not move to the first child");
    send(tree, WM_KEYDOWN, VK_LEFT);
    check(static_cast<TreeItem>(send(tree, treeGetSelection)) == directory, "Left arrow did not move to the parent");
    send(tree, WM_KEYDOWN, VK_LEFT);
    check(!expanded(tree, directory) && send(tree, treeGetCount) == 124 && nextItem(tree, TreeRelation::Child, directory) == nested,
          "Collapsing a loaded folder discarded its cached children");
    check(send(tree, treeGetRow, nested) == -1, "A cached child remained visible after collapsing its parent");
    send(tree, WM_KEYDOWN, VK_RIGHT);
    check(expanded(tree, directory) && nextItem(tree, TreeRelation::Child, directory) == nested,
          "Reopening an unchanged folder did not reuse its children immediately");
    send(tree, WM_KEYDOWN, VK_LEFT);
    send(tree, WM_KEYDOWN, VK_END);
    check(scrollInfo(tree).nPos > 0, "End did not scroll the selection into view");
    send(tree, WM_KEYDOWN, VK_HOME);
    check(static_cast<TreeItem>(send(tree, treeGetSelection)) == root && scrollInfo(tree).nPos == 0, "Home did not return to the root");
    send(tree, WM_KEYDOWN, VK_DOWN);
    check(static_cast<TreeItem>(send(tree, treeGetSelection)) == directory, "Down did not select the next visible row");
    send(tree, WM_KEYDOWN, VK_NEXT);
    check(scrollInfo(tree).nPos > 0, "Page Down did not keep the selection visible");
    send(tree, WM_KEYDOWN, VK_HOME);
    send(tree, WM_CHAR, L'f');
    check(static_cast<TreeItem>(send(tree, treeGetSelection)) == firstFile, "Typing did not select a matching file");
    send(tree, WM_CHAR, L'f');
    check(static_cast<TreeItem>(send(tree, treeGetSelection)) == nextItem(tree, TreeRelation::Next, firstFile), "Repeated typing did not cycle matching files");
    const auto horizontal = scrollInfo(tree, SB_HORZ);
    check(horizontal.nMax >= static_cast<int>(horizontal.nPage), "Long labels did not enable horizontal scrolling");
    send(tree, WM_HSCROLL, SB_BOTTOM);
    check(scrollInfo(tree, SB_HORZ).nPos > 0, "The tree did not scroll horizontally");
    send(tree, WM_HSCROLL, SB_TOP); send(tree, WM_KEYDOWN, VK_HOME);
    const int top = scrollInfo(tree).nPos;
    UINT characters = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &characters, 0);
    send(tree, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, static_cast<WORD>(-WHEEL_DELTA)));
    check(scrollInfo(tree).nPos == top && (!characters || scrollInfo(tree, SB_HORZ).nPos > 0),
        "Shift+wheel did not scroll long tree labels horizontally while preserving the vertical position");
    send(tree, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, WHEEL_DELTA));
    check(scrollInfo(tree, SB_HORZ).nPos == 0 && scrollInfo(tree).nPos == top, "Shift+wheel up did not return the tree left");
    const int row = static_cast<int>(send(tree, treeGetRow, directory)), height = static_cast<int>(send(tree, treeGetRowHeight));
    const int arrowX = MulDiv(27, GetDpiForWindow(app.window), 96), y = row * height + height / 2;
    click(tree, arrowX, y);
    waitFor([&] { return expanded(tree, directory) && nextItem(tree, TreeRelation::Child, directory); }, "Clicking the arrow did not expand the custom tree");
    click(tree, arrowX, y);
    check(!expanded(tree, directory) && nextItem(tree, TreeRelation::Child, directory) == nested,
          "Clicking the arrow did not collapse and retain cached children");
    // Real rapid clicks arrive as DOWN/UP/DBLCLK/UP, not two DOWN/UP pairs.
    // Check each press while the loaded children stay cached.
    const int labelX = MulDiv(50, GetDpiForWindow(app.window), 96);
    RECT client{}; GetClientRect(tree, &client);
    const int paddingX = client.right - MulDiv(24, GetDpiForWindow(app.window), 96);
    for (const int x : {MulDiv(4, GetDpiForWindow(app.window), 96), arrowX, labelX, paddingX}) {
        bool open = false;
        for (const auto message : {WM_LBUTTONDOWN, WM_LBUTTONDBLCLK, WM_LBUTTONDOWN, WM_LBUTTONDBLCLK}) {
            send(tree, message, MK_LBUTTON, MAKELPARAM(x, y));
            send(tree, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            open = !open;
            check(expanded(tree, directory) == open, "Folder row clicks skipped an expand/collapse press");
            check(nextItem(tree, TreeRelation::Child, directory) == nested, "Rapid toggling discarded cached children");
            if (!open) check(send(tree, treeGetRow, nested) == -1, "Rapid collapse left a child visible");
        }
        for (const UINT cancel : {WM_KEYDOWN, WM_CANCELMODE, WM_CAPTURECHANGED}) {
            send(tree, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            send(tree, cancel, cancel == WM_KEYDOWN ? VK_ESCAPE : 0);
            send(tree, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            check(!expanded(tree, directory), "A cancelled folder row click toggled the folder");
        }
    }
    click(tree, labelX, height / 2);
    check(!expanded(tree, root), "A single root row click did not collapse the tree");
    click(tree, labelX, height / 2);
    check(expanded(tree, root), "A single root row click did not reopen the tree");
}
void collapseAllSidebarTests(const fs::path& executable) {
    Fixture fixture;
    fs::create_directories(fixture.root / L"nested" / L"inner");
    writeFile(fixture.root / L"nested" / L"inner" / L"file.txt");
    {
        Application app(executable, fixture.root); const auto tree = app.tree();
        const auto root = nextItem(tree, TreeRelation::Root);
        TreeItem directory = 0, inner = 0, file = 0;
        waitFor([&] { directory = nextItem(tree, TreeRelation::Child, root); return directory != 0; }, "Collapse fixture did not load");
        expand(tree, directory);
        waitFor([&] { inner = nextItem(tree, TreeRelation::Child, directory); return expanded(tree, directory) && inner; }, "Collapse fixture folder did not expand");
        expand(tree, inner);
        waitFor([&] { file = nextItem(tree, TreeRelation::Child, inner); return expanded(tree, inner) && file; }, "Collapse fixture inner folder did not expand");
        const auto rect = bounds(app.window, tree);
        const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
        // Clicking the title must leave the tree alone. The button is on its right.
        click(app.window, rect.left + px(20), px(17));
        check(expanded(tree, root) && expanded(tree, inner), "The heading text activated Collapse All");
        // Expanded descendants can already be hidden beneath a closed ancestor.
        expand(tree, root, false);
        check(expanded(tree, directory) && expanded(tree, inner), "Fixture lost its cached expansion");
        click(app.window, rect.right - px(23), px(17));
        check(!expanded(tree, root) && !expanded(tree, directory) && !expanded(tree, inner), "Collapse All skipped hidden descendants");
        send(app.window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(rect.right - px(23), px(17)));
        send(app.window, WM_LBUTTONUP);
        check(!expanded(tree, root), "Repeating Collapse All reopened the root");
        expand(tree, root);
        check(!expanded(tree, directory), "Reopening the root expanded a collapsed descendant");
        expand(tree, directory); expand(tree, inner); send(tree, treeSelect, file);
        click(app.window, rect.right - px(23), px(17));
        check(send(tree, treeGetSelection) == static_cast<LRESULT>(root) && scrollInfo(tree).nPos == 0 && scrollInfo(tree, SB_HORZ).nPos == 0,
            "Collapse All left an invisible selection or stale scroll position");
        send(app.window, WM_KEYDOWN, VK_F5);
        check(!expanded(tree, root), "Refreshing reopened the collapsed root");
    }
    {
        Application app(executable, fixture.root); const auto tree = app.tree();
        const auto root = nextItem(tree, TreeRelation::Root);
        check(!expanded(tree, root) && !nextItem(tree, TreeRelation::Child, root), "Restart forgot Collapse All in Explorer");
    }
}
void explorerSortTests(const fs::path& executable) {
    Fixture fixture;
    for (const auto number : {11, 2}) {
        const auto folder = fixture.root / (std::to_wstring(number) + L"_folder");
        fs::create_directory(folder); writeLines(folder / L"nested.txt", number + 100);
    }
    for (const auto number : {11, 4, 3, 2, 1}) writeLines(fixture.root / (std::to_wstring(number) + L"_file.txt"), number + 200);
    writeLines(fixture.root / L"a2.txt", 302); writeLines(fixture.root / L"A11.txt", 311);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 10; }, "Natural sort fixture did not load");
    const auto root = nextItem(tree, TreeRelation::Root);
    auto folder = nextItem(tree, TreeRelation::Child, root);
    for (const auto lines : {102, 111}) {
        check(folder != 0, "Natural sort hid a folder");
        expand(tree, folder);
        waitFor([&] { return nextItem(tree, TreeRelation::Child, folder) != 0; }, "Sorted folder did not expand");
        send(tree, treeSelect, nextItem(tree, TreeRelation::Child, folder)); waitLines(viewer, lines);
        folder = nextItem(tree, TreeRelation::Next, folder);
    }
    const auto checkFiles = [&](std::initializer_list<int> expected) {
        const auto firstFolder = nextItem(tree, TreeRelation::Child, root);
        auto item = nextItem(tree, TreeRelation::Next, nextItem(tree, TreeRelation::Next, firstFolder));
        for (const auto lines : expected) {
            check(item != 0, "Natural sort hid a file");
            send(tree, treeSelect, item); waitLines(viewer, lines);
            item = nextItem(tree, TreeRelation::Next, item);
        }
        check(!item, "Natural sort produced unexpected siblings");
    };
    checkFiles({201, 202, 203, 204, 211, 302, 311});
    // Rename through Poppy to exercise the immediate re-sort after relocation.
    const auto firstFolder = nextItem(tree, TreeRelation::Child, root);
    const auto firstFile = nextItem(tree, TreeRelation::Next, nextItem(tree, TreeRelation::Next, firstFolder));
    send(tree, treeSelect, firstFile); waitLines(viewer, 201);
    send(tree, WM_KEYDOWN, VK_F2);
    const auto edit = FindWindowExW(tree, nullptr, treeInputClassName, nullptr);
    check(edit && SetWindowTextW(edit, L"12_file.txt"), "Could not enter a numeric filename");
    send(edit, WM_KEYDOWN, VK_RETURN);
    waitFor([&] { return fs::exists(fixture.root / L"12_file.txt") && nextItem(tree, TreeRelation::Next, firstFile) != 0 &&
        nextItem(tree, TreeRelation::Next, nextItem(tree, TreeRelation::Next, firstFolder)) != firstFile; }, "Renaming did not re-sort numeric filenames");
    checkFiles({202, 203, 204, 211, 201, 302, 311});
    // Wait for a new enumeration rather than checking the cached order.
    const auto count = send(tree, treeGetCount);
    writeLines(fixture.root / L"10_file.txt", 210);
    send(app.window, WM_KEYDOWN, VK_F5);
    waitFor([&] { return send(tree, treeGetCount) == count + 1; }, "Refresh did not enumerate the added numeric filename");
    checkFiles({202, 203, 204, 210, 211, 201, 302, 311});
}
void folderTests(const fs::path& executable) {
    Fixture fixture;
    const auto empty = fixture.root / L"a empty";
    const auto nested = fixture.root / L"b nested";
    fs::create_directory(empty); fs::create_directory(nested);
    writeFile(nested / L"日本語 file.txt");
    {
        Application app(executable, fixture.root);
        const auto tree = app.tree();
        TreeItem root = 0;
        waitFor([&] {
            root = nextItem(tree, TreeRelation::Root);
            return root && expanded(tree, root) && nextItem(tree, TreeRelation::Child, root);
        }, "Opening a workspace did not display its files");
        const auto emptyItem = nextItem(tree, TreeRelation::Child, root);
        const auto nestedItem = nextItem(tree, TreeRelation::Next, emptyItem);
        check(nestedItem && !nextItem(tree, TreeRelation::Next, nestedItem), "Workspace should contain two directories");
        for (int i = 0; i < 2; ++i) {
            expand(tree, nestedItem);
            waitFor([&] { return expanded(tree, nestedItem) && nextItem(tree, TreeRelation::Child, nestedItem); },
                    "Expanding a directory did not display its files");
            const auto child = nextItem(tree, TreeRelation::Child, nestedItem);
            expand(tree, nestedItem, false);
            check(!expanded(tree, nestedItem) && nextItem(tree, TreeRelation::Child, nestedItem) == child,
                  "Collapsing a directory discarded its cached children");
        }
        writeFile(nested / L"added while collapsed.txt");
        check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not invalidate a collapsed folder");
        expand(tree, nestedItem);
        waitFor([&] { return nextItem(tree, TreeRelation::Next, nextItem(tree, TreeRelation::Child, nestedItem)) != 0; },
                "Reopening a changed cached folder did not display the added file");
        expand(tree, emptyItem);
        // Watch notifications and F5 must also invalidate an empty folder.
        expand(tree, nestedItem);
        waitFor([&] { return expanded(tree, nestedItem) && nextItem(tree, TreeRelation::Child, nestedItem); },
                "The cached directory did not reopen");
        writeFile(empty / L"new file.txt");
        check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not request a refresh");
        waitFor([&] { return expanded(tree, emptyItem) && nextItem(tree, TreeRelation::Child, emptyItem); },
                "A file added to an empty directory did not appear");
    }
    {
        Fixture emptyRoot;
        Application app(executable, emptyRoot.root);
        const auto tree = app.tree();
        TreeItem root = 0;
        waitFor([&] { root = nextItem(tree, TreeRelation::Root); return root != 0; }, "Empty workspace has no root item");
        writeFile(emptyRoot.root / L"first file.txt");
        check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh the empty workspace");
        waitFor([&] { return expanded(tree, root) && nextItem(tree, TreeRelation::Child, root); },
                "A file added to an empty workspace did not appear");
    }
}
void quickOpenTests(const fs::path& executable) {
    Fixture fixture;
    fs::create_directories(fixture.root / L"a nested"); fs::create_directories(fixture.root / L"z nested");
    writeLines(fixture.root / L"a nested" / L"duplicate.txt", 201);
    writeLines(fixture.root / L"z nested" / L"duplicate.txt", 301);
    writeLines(fixture.root / L"a nested" / L"日本語.txt", 101);
    Application app(executable, fixture.root);
    app.tree();
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    const auto palette = FindWindowExW(app.window, nullptr, L"PoppyQuickOpen", nullptr);
    check(viewer && palette && !IsWindowVisible(palette), "Quick open should start hidden");
    const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
    const auto open = [&] {
        controlShortcut(app, viewer, 'P', [&] { return IsWindowVisible(palette); }, "Ctrl+P did not open quick open");
        const auto pane = bounds(app.window, palette); RECT client{}; GetClientRect(app.window, &client);
        check(pane.top == px(8) && std::abs((pane.left + pane.right) - client.right) <= 1, "Quick open is not centered at the top");
    };
    const auto type = [&](const std::wstring& query) { for (const auto ch : query) send(palette, WM_CHAR, ch); };
    const auto accept = [&](int lines) {
        waitFor([&] { send(palette, WM_KEYDOWN, VK_RETURN); return !IsWindowVisible(palette); }, "Quick open did not open a matching file");
        waitLines(viewer, lines);
    };
    open(); type(L"duplicate");
    waitFor([&] {
        click(palette, px(24), px(86 + 44 + 22));
        return !IsWindowVisible(palette);
    }, "Quick open did not open the second match from collapsed directories");
    waitLines(viewer, 301); keepTab(app, 0);
    open(); type(L"missing-file"); send(palette, WM_KEYDOWN, VK_RETURN);
    check(IsWindowVisible(palette), "Enter opened a stale result while a new query was pending");
    send(palette, WM_KEYDOWN, VK_ESCAPE);
    check(!IsWindowVisible(palette) && lastTextRow(viewer) == 301, "Escape changed the current document");
    open(); type(L"日本語"); accept(101);
    // Opening the same path again should switch to its tab instead of duplicating it.
    open(); type(L"日本語"); accept(101);
    auto pane = bounds(app.window, viewer);
    click(app.window, pane.left + px(210), pane.top - px(17), true);
    waitLines(viewer, 301);
    pane = bounds(app.window, viewer);
    click(app.window, pane.left + px(30), pane.top - px(17), true);
    check(scrollInfo(viewer).nMax == 0, "Quick open created duplicate tabs");
    // Directory changes refresh the cached list even while the palette is hidden.
    writeLines(fixture.root / L"after-startup.txt", 401);
    open(); type(L"after-startup"); accept(401);
    fs::remove(fixture.root / L"after-startup.txt");
    writeLines(fixture.root / L"after-delete.txt", 501);
    open(); type(L"after-delete"); accept(501);
    open(); type(L"after-startup"); send(palette, WM_KEYDOWN, VK_RETURN);
    check(IsWindowVisible(palette), "A refreshed file list retained a deleted path");
    send(palette, WM_KEYDOWN, VK_ESCAPE);
    open();
    click(app.window, px(12), px(22));
    check(!IsWindowVisible(palette), "Clicking outside quick open did not close it");
}
void explorerActiveFileTests(const fs::path& executable) {
    Fixture fixture;
    fs::create_directories(fixture.root / L"a nested" / L"inner");
    fs::create_directories(fixture.root / L"z nested" / L"inner");
    writeLines(fixture.root / L"a nested" / L"inner" / L"日本語.txt", 101);
    writeLines(fixture.root / L"z nested" / L"inner" / L"second.txt", 201);
    for (int i = 0; i < 20; ++i) fs::create_directory(fixture.root / (L"b filler-" + std::to_wstring(i)));
    for (int i = 0; i < 40; ++i) writeFile(fixture.root / (L"filler-" + std::to_wstring(i) + L".txt"));
    writeLines(fixture.root / L"filler-9.txt", 301);
    Application app(executable, fixture.root); const auto tree = app.tree();
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    const auto palette = FindWindowExW(app.window, nullptr, L"PoppyQuickOpen", nullptr);
    const auto gitButton = FindWindowExW(app.window, nullptr, activityButtonClassName, L"Source Control");
    const auto explorerButton = FindWindowExW(app.window, nullptr, activityButtonClassName, L"Explorer");
    check(viewer && palette && gitButton && explorerButton, "Explorer selection panes are missing");
    waitFor([&] { return send(tree, treeGetCount) == 63; }, "Explorer selection fixture did not load");
    const auto root = nextItem(tree, TreeRelation::Root), firstFolder = nextItem(tree, TreeRelation::Child, root);
    auto secondFolder = nextItem(tree, TreeRelation::Next, firstFolder);
    for (int i = 0; i < 20; ++i) secondFolder = nextItem(tree, TreeRelation::Next, secondFolder);
    const auto open = [&](const wchar_t* name, int lines) {
        controlShortcut(app, viewer, 'P', [&] { return IsWindowVisible(palette); }, "Cannot open file search for Explorer selection");
        for (const auto ch : std::wstring(name)) send(palette, WM_CHAR, ch);
        waitFor([&] { send(palette, WM_KEYDOWN, VK_RETURN); return !IsWindowVisible(palette); }, "Cannot open selection fixture file");
        waitLines(viewer, lines);
    };
    const auto selection = [&] { return static_cast<TreeItem>(send(tree, treeGetSelection)); };
    const auto checkPosition = [&] {
        const int row = static_cast<int>(send(tree, treeGetRow, selection()));
        const auto scroll = scrollInfo(tree);
        const int page = static_cast<int>(scroll.nPage);
        check(row >= scroll.nPos && row < scroll.nPos + page, "Active file selection is outside the visible rows");
        const int offset = row - scroll.nPos;
        const bool centered = offset == page / 2;
        const bool atStart = scroll.nPos == scroll.nMin && offset < page / 2;
        const bool atEnd = scroll.nPos + page > scroll.nMax && offset > page / 2;
        check(centered || atStart || atEnd, "Active file selection was not centered within the scroll range");
    };
    const auto checkSelected = [&](TreeItem folder) {
        waitFor([&] {
            const auto inner = nextItem(tree, TreeRelation::Child, folder);
            const auto file = nextItem(tree, TreeRelation::Child, inner);
            return expanded(tree, root) && expanded(tree, folder) && expanded(tree, inner) && file && selection() == file;
        }, "Explorer did not expand and select the active file");
        check(send(tree, treeGetSelectionCount) == 1, "Active file reveal left multiple rows selected");
        checkPosition();
    };
    send(tree, WM_VSCROLL, SB_BOTTOM);
    open(L"日本語", 101); checkSelected(firstFolder); keepTab(app, 0);
    // Hidden Explorer follows file changes without switching the sidebar.
    send(gitButton, BM_CLICK); check(!IsWindowVisible(tree), "Source Control did not hide Explorer");
    open(L"second", 201); checkSelected(secondFolder);
    check(!IsWindowVisible(tree), "Opening a file unexpectedly switched the sidebar");
    send(explorerButton, BM_CLICK); checkSelected(secondFolder);
    // Recenter an already selected row even when it remains visible.
    send(tree, WM_VSCROLL, SB_LINEDOWN);
    clickTab(app, 1); checkSelected(secondFolder);
    // Cached descendants must also be revealed through a collapsed root.
    expand(tree, root, false);
    clickTab(app, 0); waitLines(viewer, 101); checkSelected(firstFolder);
    expand(tree, firstFolder, false);
    clickTab(app, 0); checkSelected(firstFolder);
    // A later tab activation supersedes any earlier asynchronous reveal.
    expand(tree, root, false);
    send(app.window, WM_KEYDOWN, VK_F5);
    clickTab(app, 1); clickTab(app, 0); clickTab(app, 1);
    waitLines(viewer, 201); checkSelected(secondFolder);
    clickTab(app, 1, true); waitLines(viewer, 101); checkSelected(firstFolder);
    // Navigation in Explorer remains available after automatic selection.
    expand(tree, secondFolder, false);
    send(viewer, WM_SETFOCUS);
    check(selection() == secondFolder, "Refocusing the same editor overwrote manual tree navigation");
    // The final file stops at the end of the list instead of overscrolling.
    open(L"filler-9", 301); checkPosition();
    check(send(tree, treeGetRow, selection()) == scrollInfo(tree).nMax, "Bottom-edge fixture did not select the final row");
}
void fullSearchTests(const fs::path& executable) {
    Fixture fixture; fs::create_directory(fixture.root / L"ignored");
    {
        std::ofstream file(fixture.root / L"file.txt");
        for (int line = 0; line < 300; ++line) file << (line == 220 ? "unique-needle" : "plain text") << '\n';
        std::ofstream ignore(fixture.root / L".gitignore"); ignore << "ignored/\n";
        std::ofstream hidden(fixture.root / L"ignored" / L"hidden.txt"); hidden << "unique-needle\n";
    }
    Application app(executable, fixture.root); app.tree();
    const auto button = FindWindowExW(app.window, nullptr, activityButtonClassName, L"Search");
    const auto pane = FindWindowExW(app.window, nullptr, L"PoppySearchPane", nullptr);
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    check(button && pane && !IsWindowVisible(pane), "Search sidebar should start hidden");
    send(button, BM_CLICK);
    const auto input = FindWindowExW(pane, nullptr, L"PoppyFindBar", nullptr);
    check(IsWindowVisible(pane) && input, "Search activity did not open its input");
    const auto type = [&](const std::wstring& query) { for (const auto ch : query) send(input, WM_CHAR, ch); };
    const auto finished = [&](size_t count) {
        waitFor([&] { wchar_t text[1024]{}; GetWindowTextW(pane, text, 1024); const std::wstring title = text;
            return title.find(L"Search project: " + std::to_wstring(count) + L" matches") == 0 && title.find(L"Searching") == title.npos;
        }, "Project search did not publish its expected results");
    };
    type(L"unique-needle"); finished(1);
    send(input, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
    check(scrollInfo(viewer).nPos > 150, "Search result did not reveal the matching line");
    // Editing and saving keep the old results; only an explicit search uses
    // the current contents. Dirty buffers must still override disk contents.
    send(viewer, WM_CHAR, '!'); finished(1);
    controlShortcut(app, viewer, 'S', [&] { return readText(fixture.root / L"file.txt").find('!') != std::string::npos; }, "Could not save an edited search result");
    finished(1);
    check(PostMessageW(input, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh edited project search"); finished(0);
    send(button, BM_CLICK); type(L"!"); finished(1);
    send(input, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
    send(viewer, WM_CHAR, '\r'); waitLines(viewer, 301); finished(1);
    check(PostMessageW(input, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh project search");
    finished(0);
    controlShortcut(app, viewer, 'S', [&] { return readText(fixture.root / L"file.txt").find('!') == std::string::npos; }, "Could not save the final search fixture");
}
void searchRetentionTests(const fs::path& executable) {
    Fixture first, second;
    {
        std::ofstream file(first.root / L"matches.txt");
        for (int i = 0; i < 80; ++i) file << "needle " << i << '\n';
        std::ofstream other(second.root / L"other.txt"); other << "second needle\n";
    }
    const auto title = [](HWND pane) { wchar_t text[1024]{}; GetWindowTextW(pane, text, 1024); return std::wstring(text); };
    const auto finished = [&](HWND pane, size_t count) {
        waitFor([&] { const auto text = title(pane); return text.find(L"Search project: " + std::to_wstring(count) + L" matches") == 0 && text.find(L"Searching") == text.npos; },
            "Retained search did not publish its expected results");
    };
    const auto type = [](HWND input, const std::wstring& query) { for (const auto ch : query) send(input, WM_CHAR, ch); };
    int top = 0;
    std::wstring savedTitle;
    {
        Application app(executable, first.root); app.tree();
        const auto surface = app.window;
        const auto button = FindWindowExW(surface, nullptr, activityButtonClassName, L"Search");
        const auto pane = FindWindowExW(surface, nullptr, L"PoppySearchPane", nullptr);
        const auto input = FindWindowExW(pane, nullptr, L"PoppyFindBar", nullptr);
        const auto viewer = FindWindowExW(surface, nullptr, L"PoppyViewer", nullptr);
        send(button, BM_CLICK); type(input, L"needle"); finished(pane, 80);
        send(pane, WM_KEYDOWN, VK_END); top = scrollInfo(pane).nPos;
        check(top > 0, "Search retention fixture did not scroll");
        send(pane, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 80);
        savedTitle = title(pane);
        // Wait for the normal file watch to refresh the open document. The
        // search must keep its original rows, status, selection and scroll.
        writeLines(first.root / L"matches.txt", 300); waitLines(viewer, 300);
        check(title(pane) == savedTitle && scrollInfo(pane).nPos == top, "A file event changed search results");
        clickTab(app, 0, true);
        check(title(pane) == savedTitle && scrollInfo(pane).nPos == top, "Closing a tab changed search results");
        const auto path = second.root.native();
        COPYDATASTRUCT data{0x50505901, static_cast<DWORD>((path.size()+1)*sizeof(wchar_t)), const_cast<wchar_t*>(path.c_str())};
        check(send(app.host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)), "Cannot open a second search project");
        app.tree(); check(app.window != surface, "Search test did not switch projects");
        const auto otherPane = FindWindowExW(app.window, nullptr, L"PoppySearchPane", nullptr);
        send(FindWindowExW(app.window, nullptr, activityButtonClassName, L"Search"), BM_CLICK);
        type(FindWindowExW(otherPane, nullptr, L"PoppyFindBar", nullptr), L"second"); finished(otherPane, 1);
        const auto px = [dpi = GetDpiForWindow(app.host)](int value) { return MulDiv(value, dpi, 96); };
        click(app.host, px(60), px(68)); app.tree();
        check(app.window == surface && title(pane) == savedTitle && scrollInfo(pane).nPos == top, "Switching projects reset search results");
        send(FindWindowExW(surface, nullptr, activityButtonClassName, L"Explorer"), BM_CLICK); send(button, BM_CLICK);
        check(title(pane) == savedTitle && scrollInfo(pane).nPos == top, "Switching sidebars reset search results");
        send(input, WM_KEYDOWN, VK_ESCAPE); send(button, BM_CLICK);
        check(title(pane) == savedTitle, "Esc replaced a completed search status");
    }
    {
        Application app(executable, first.root); app.tree();
        const auto pane = FindWindowExW(app.window, nullptr, L"PoppySearchPane", nullptr);
        check(IsWindowVisible(pane) && title(pane) == savedTitle && scrollInfo(pane).nPos == top, "Restart did not restore the retained search and view");
        // Moving down at the end must retain the restored selection and top.
        send(pane, WM_KEYDOWN, VK_DOWN);
        check(scrollInfo(pane).nPos == top, "Restart lost the selected search result");
        const auto input = FindWindowExW(pane, nullptr, L"PoppyFindBar", nullptr);
        check(PostMessageW(input, WM_KEYDOWN, VK_F5, 0) != FALSE, "Cannot rerun a restored query"); finished(pane, 0);
        const auto px = [dpi = GetDpiForWindow(app.host)](int value) { return MulDiv(value, dpi, 96); };
        click(app.host, px(60), px(44 + projectRowHeight + 24)); app.tree();
        const auto otherPane = FindWindowExW(app.window, nullptr, L"PoppySearchPane", nullptr);
        check(IsWindowVisible(otherPane), "Restart lost the other project's search sidebar"); finished(otherPane, 1);
    }
}
void gitSidebarTests(const fs::path& executable) {
    wchar_t git[MAX_PATH]{};
    const auto length = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, git, nullptr);
    if (!length || length >= MAX_PATH) { std::cout << "Git sidebar tests skipped: Git for Windows is not on PATH.\n"; return; }
    Fixture fixture;
    fs::create_directories(fixture.root / L"src" / L"nested 日本語");
    fs::create_directory(fixture.root / L"docs");
    writeLines(fixture.root / L"docs" / L"same.txt", 200);
    writeLines(fixture.root / L"src" / L"nested 日本語" / L"same.txt", 300);
    writeLines(fixture.root / L"root.txt", 400);
    std::wstring command = L"\"" + std::wstring(git) + L"\" init --quiet --template=\"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(git, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, fixture.root.c_str(), &startup, &process) != FALSE,
        "Could not initialize the Git fixture");
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 5000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
    check(wait == WAIT_OBJECT_0 && code == 0, "Git fixture initialization failed");
    // Keep the fixture independent of the user's global ignore patterns.
    {
        std::ofstream config(fixture.root / L".git" / L"config", std::ios::app);
        config << "\n[core]\n\texcludesFile =\n";
        config.close(); check(!config.fail(), "Could not isolate the Git fixture's ignore rules");
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto changes = GetDlgItem(app.window, 4), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        send(GetDlgItem(app.window, 3), BM_CLICK);
        waitFor([&] { return IsWindowVisible(changes) && gitChangeCount(changes) == 8; }, "Default Git Tree did not load all untracked files");
        const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
        const auto view = [&](bool tree) {
            const auto rect = bounds(app.window, changes);
            click(app.window, rect.left + (rect.right - rect.left) * (tree ? 1 : 3) / 4, rect.top - px(14));
            check(gitChangeCount(changes) == (tree ? 8 : 5), "Git layout toggle did not rebuild the hierarchy");
        };
        view(false); view(true);
        auto group = gitGroup(changes, false);
        auto docs = nextItem(changes, TreeRelation::Child, group), src = nextItem(changes, TreeRelation::Next, docs);
        check(expanded(changes, docs) && expanded(changes, src), "Git folders did not start expanded");
        send(changes, treeSelect, group); send(changes, WM_KEYDOWN, VK_F4); waitLines(viewer, 905, true);
        expand(changes, src, false); waitLines(viewer, 603, true);
        expand(changes, src); waitLines(viewer, 905, true);
        const auto visibilityNested = nextItem(changes, TreeRelation::Child, src);
        expand(changes, visibilityNested, false); waitLines(viewer, 603, true);
        expand(changes, visibilityNested); waitLines(viewer, 905, true);
        const auto rootFile = nextItem(changes, TreeRelation::Next, src);
        const auto toggleEye = [&] {
            RECT rect{}; GetClientRect(changes, &rect);
            const int height = static_cast<int>(send(changes, treeGetRowHeight));
            const int y = (static_cast<int>(send(changes, treeGetRow, rootFile)) - scrollInfo(changes).nPos) * height + height / 2;
            // The eye appears on hover; it follows Stage,
            // Discard and Open File without changing their hit regions.
            send(changes, WM_MOUSELEAVE);
            click(changes, rect.right - px(102), y);
        };
        toggleEye(); waitLines(viewer, 503, true);
        check(visibleTabCount(app) == 1, "Toggling visibility opened a file tab");
        expand(changes, src, false); waitLines(viewer, 201, true);
        view(false); waitLines(viewer, 503, true);
        view(true); waitLines(viewer, 201, true);
        // Layout changes rebuild item IDs but keep each file's visibility.
        group = gitGroup(changes, false);
        docs = nextItem(changes, TreeRelation::Child, group); src = nextItem(changes, TreeRelation::Next, docs);
        const auto restoredRootFile = nextItem(changes, TreeRelation::Next, src);
        send(changes, treeSelect, restoredRootFile); waitLines(viewer, 400);
        send(changes, treeSelect, group); send(changes, WM_KEYDOWN, VK_F4); waitLines(viewer, 201, true);
        expand(changes, docs, false); waitLines(viewer, 0);
        expand(changes, docs); waitLines(viewer, 201, true);
        // Show the root file again using its newly allocated row.
        RECT visibilityRect{}; GetClientRect(changes, &visibilityRect);
        const int visibilityHeight = static_cast<int>(send(changes, treeGetRowHeight));
        const int visibilityY = (static_cast<int>(send(changes, treeGetRow, restoredRootFile)) - scrollInfo(changes).nPos) * visibilityHeight + visibilityHeight / 2;
        click(changes, visibilityRect.right - px(102), visibilityY); waitLines(viewer, 603, true);
        expand(changes, src); waitLines(viewer, 905, true);
        clickTab(app, 0, false, true);
        send(changes, treeSelect, docs);
        check(scrollInfo(viewer).nMax == 0, "Selecting a Git folder opened a file");
        send(changes, treeSelect, nextItem(changes, TreeRelation::Child, docs)); waitLines(viewer, 200);
        const auto clickFolder = [&](UINT message, int x) {
            const int height = static_cast<int>(send(changes, treeGetRowHeight));
            const int y = (static_cast<int>(send(changes, treeGetRow, src)) - scrollInfo(changes).nPos) * height + height / 2;
            send(changes, message, MK_LBUTTON, MAKELPARAM(px(x), y));
            send(changes, WM_LBUTTONUP, 0, MAKELPARAM(px(x), y));
        };
        for (const int x : {4, 90}) {
            bool open = true;
            for (const auto message : {WM_LBUTTONDOWN, WM_LBUTTONDBLCLK, WM_LBUTTONDOWN, WM_LBUTTONDBLCLK}) {
                clickFolder(message, x); open = !open;
                check(expanded(changes, src) == open, "Git folder row clicks skipped an expand/collapse press");
            }
        }
        check(visibleTabCount(app) == 1, "Clicking a Git folder opened another file tab");
        clickFolder(WM_LBUTTONDOWN, 90);
        check(!expanded(changes, src), "A single Git folder row click did not collapse the folder");
        view(false); view(true);
        group = gitGroup(changes, false);
        docs = nextItem(changes, TreeRelation::Child, group); src = nextItem(changes, TreeRelation::Next, docs);
        check(!expanded(changes, src), "Changing layouts forgot a collapsed Git folder");
        check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh Git changes");
        waitFor([&] { return gitGroup(changes, false) != group; }, "Git changes did not refresh");
        group = gitGroup(changes, false);
        docs = nextItem(changes, TreeRelation::Child, group); src = nextItem(changes, TreeRelation::Next, docs);
        check(!expanded(changes, src), "Refreshing forgot a collapsed Git folder");
        expand(changes, src);
        const auto nested = nextItem(changes, TreeRelation::Child, src), file = nextItem(changes, TreeRelation::Child, nested);
        check(file != 0, "Git Tree lost a nested untracked file");
        send(changes, treeSelect, file); waitLines(viewer, 300);
        check(visibleTabCount(app) == 1, "Browsing Source Control files accumulated preview tabs");
        view(false);
        send(changes, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
        view(true);
        group = gitGroup(changes, false);
        docs = nextItem(changes, TreeRelation::Child, group); src = nextItem(changes, TreeRelation::Next, docs);
        const auto branch = gitBranch(changes);
        const auto inner = nextItem(changes, TreeRelation::Child, src);
        const auto rect = bounds(app.window, changes);
        click(app.window, rect.right - px(23), px(17));
        check(!expanded(changes, branch) && !expanded(changes, group) && !expanded(changes, docs) &&
            !expanded(changes, src) && !expanded(changes, inner), "Source Control Collapse All skipped a branch, group or nested folder");
        check(gitChangeCount(changes) == 8 && scrollInfo(changes).nPos == 0, "Source Control Collapse All changed entries or retained a stale scroll position");
        expand(changes, branch);
        check(!expanded(changes, group), "Reopening the branch reopened the Changes group");
        view(false); view(true);
        group = gitGroup(changes, false);
        check(!expanded(changes, group) && !expanded(changes, nextItem(changes, TreeRelation::Child, group)),
            "Switching Git layouts forgot Collapse All");
        const auto beforeRefresh = group;
        send(app.window, WM_KEYDOWN, VK_F5);
        waitFor([&] { return gitGroup(changes, false) != beforeRefresh; }, "Collapsed Git changes did not refresh");
        check(!expanded(changes, gitGroup(changes, false)), "Refreshing reopened a collapsed Changes group");
        click(app.window, rect.right - px(23), px(17));
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto changes = GetDlgItem(app.window, 4);
        waitFor([&] { return IsWindowVisible(changes) && gitChangeCount(changes) == 8; }, "Restart lost the Git Tree layout");
        check(!expanded(changes, gitBranch(changes)) && !expanded(changes, gitGroup(changes, false)),
            "Restart forgot Collapse All in Source Control");
        const auto rect = bounds(app.window, changes);
        click(app.window, rect.left + (rect.right - rect.left) * 3 / 4, rect.top - MulDiv(14, GetDpiForWindow(app.window), 96));
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto changes = GetDlgItem(app.window, 4);
        waitFor([&] { return IsWindowVisible(changes) && gitChangeCount(changes) == 5; }, "Restart lost the Git List layout");
    }
}
void gitIgnoredExplorerTests(const fs::path& executable) {
    wchar_t git[MAX_PATH]{};
    const auto length = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, git, nullptr);
    if (!length || length >= MAX_PATH) { std::cout << "Git ignore GUI tests skipped: Git for Windows is not on PATH.\n"; return; }
    Fixture fixture;
    const auto runGit = [&](const std::wstring& args) {
        std::wstring command = L"\"" + std::wstring(git) + L"\" " + args;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        check(CreateProcessW(git, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, fixture.root.c_str(), &startup, &process) != FALSE, "Cannot start ignore fixture Git");
        CloseHandle(process.hThread); const auto wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
        check(wait == WAIT_OBJECT_0 && !code, "Ignore fixture Git failed");
    };
    runGit(L"init --quiet --template=\"\""); runGit(L"config core.excludesFile \"\"");
    writeLines(fixture.root / L"tracked.tmp", 100); runGit(L"add -- tracked.tmp");
    fs::create_directory(fixture.root / L"a-ignored"); writeLines(fixture.root / L"a-ignored" / L"child.txt", 200);
    for (const auto name : {L"a.tmp", L"keep.tmp", L"visible.txt"}) writeLines(fixture.root / name, 100);
    const auto rules = [&](const char* text) { std::ofstream file(fixture.root / L".gitignore", std::ios::binary); file << text; file.close(); check(!file.fail(), "Cannot write ignore rules"); };
    rules("*.tmp\n!keep.tmp\na-ignored/\n");
    Application app(executable, fixture.root); const auto tree = app.tree();
    const auto root = nextItem(tree, TreeRelation::Root);
    std::vector<TreeItem> items;
    waitFor([&] {
        items.clear();
        for (auto item = nextItem(tree, TreeRelation::Child, root); item; item = nextItem(tree, TreeRelation::Next, item)) items.push_back(item);
        return items.size() == 7;
    }, "Ignore fixture did not load");
    const auto dimmed = [&](TreeItem item) { return send(tree, treeGetColor, item) == RGB(132, 145, 164); };
    // Folders precede files: .git, a-ignored, .gitignore, a.tmp, keep.tmp,
    // tracked.tmp, visible.txt. Tracked names retain their Git status colors.
    waitFor([&] { return dimmed(items[1]) && dimmed(items[3]) && !dimmed(items[4]) && !dimmed(items[5]) && !dimmed(items[6]); }, "Ignored Explorer names did not use the muted color");
    expand(tree, items[1]); TreeItem child = 0;
    waitFor([&] { child = nextItem(tree, TreeRelation::Child, items[1]); return child && dimmed(child); }, "An ignored folder's child was not dimmed");
    send(tree, treeSelect, child); waitLines(FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr), 200);
    rules("*.tmp\n");
    waitFor([&] { return !dimmed(items[1]) && !dimmed(child) && dimmed(items[4]); }, "Editing ignore rules did not update retained Explorer names");
    check(expanded(tree, items[1]) && send(tree, treeGetSelection) == static_cast<LRESULT>(child), "Recoloring changed expansion or selection");
    runGit(L"add --force -- a.tmp");
    waitFor([&] { return !dimmed(items[3]); }, "A newly tracked ignored file stayed dimmed");
    rules("");
    waitFor([&] { return !dimmed(items[4]); }, "Removing ignore rules left a file dimmed");
}
void gitAutoRefreshTests(const fs::path& executable) {
    wchar_t git[MAX_PATH]{};
    const auto length = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, git, nullptr);
    if (!length || length >= MAX_PATH) { std::cout << "Git auto refresh tests skipped: Git for Windows is not on PATH.\n"; return; }
    Fixture fixture;
    const auto runGit = [&](const std::wstring& args) {
        std::wstring command = L"\"" + std::wstring(git) + L"\" " + args;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        check(CreateProcessW(git, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, fixture.root.c_str(), &startup, &process) != FALSE, "Cannot start fixture Git");
        CloseHandle(process.hThread); const auto wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
        check(wait == WAIT_OBJECT_0 && code == 0, "Auto refresh fixture Git command failed");
    };
    runGit(L"init --quiet --template=\"\"");
    runGit(L"config user.name PoppyTest"); runGit(L"config user.email poppy@example.invalid");
    runGit(L"config commit.gpgsign false"); runGit(L"config core.autocrlf false");
    runGit(L"config core.excludesFile \"\"");
    writeLines(fixture.root / L"tracked.txt", 200);
    runGit(L"add ."); runGit(L"commit -qm initial");
    Application app(executable, fixture.root); app.tree();
    const auto changes = GetDlgItem(app.window, 4), button = GetDlgItem(app.window, 3);
    const auto group = [&](bool staged) {
        const auto first = gitGroup(changes, true);
        return staged ? first : nextItem(changes, TreeRelation::Next, first);
    };
    const auto firstFile = [&](bool staged) {
        auto item = nextItem(changes, TreeRelation::Child, group(staged));
        while (item) {
            const auto child = nextItem(changes, TreeRelation::Child, item);
            if (!child) break;
            item = child;
        }
        return item;
    };
    const auto clickAction = [&](TreeItem item) {
        check(item != 0, "The Git file action is missing its file");
        RECT rect{}; GetClientRect(changes, &rect);
        const int height = static_cast<int>(send(changes, treeGetRowHeight));
        const int y = (static_cast<int>(send(changes, treeGetRow, item)) - scrollInfo(changes).nPos) * height + height / 2;
        const int x = rect.right - MulDiv(18, GetDpiForWindow(app.window), 96);
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        click(changes, x, y);
        send(changes, WM_MOUSELEAVE);
    };
    const auto badge = [&](size_t count) {
        wchar_t title[128]{}; GetWindowTextW(button, title, 128);
        return title == (count ? L"Source Control (" + std::to_wstring(count) + L")" : std::wstring(L"Source Control"));
    };
    send(button, BM_CLICK);
    waitFor([&] { return badge(0) && gitChangeCount(changes) == 2 && IsWindowVisible(changes); }, "Clean repository did not retain its branch and history without a badge");
    send(GetDlgItem(app.window, 2), BM_CLICK);
    writeLines(fixture.root / L"tracked.txt", 210);
    waitFor([&] { return badge(1) && gitChangeCount(changes) == 3; }, "External edits did not update the hidden Source Control list and badge");
    send(button, BM_CLICK);
    waitFor([&] { return IsWindowVisible(changes); }, "Source Control did not show external edits");
    clickAction(firstFile(false));
    waitFor([&] { return nextItem(changes, TreeRelation::Child, gitGroup(changes, true)) != 0; }, "Staging did not automatically update Source Control");
    clickAction(firstFile(true));
    waitFor([&] { return badge(1) && !firstFile(true) && firstFile(false) && gitChangeCount(changes) == 3; }, "The minus icon did not move the file back to Changes");
    clickAction(firstFile(false));
    waitFor([&] { return firstFile(true) && !firstFile(false) && gitChangeCount(changes) == 3; }, "The plus icon did not move the file to Staged Changes");
    writeLines(fixture.root / L"tracked.txt", 220);
    waitFor([&] { return badge(1) && gitChangeCount(changes) == 4; }, "A partially staged file was not refreshed or was counted twice");
    clickAction(firstFile(false));
    waitFor([&] { return badge(1) && firstFile(true) && !firstFile(false) && gitChangeCount(changes) == 3; }, "Staging a partially staged file did not update both groups");
    clickAction(firstFile(true));
    waitFor([&] { return badge(1) && !firstFile(true) && firstFile(false) && gitChangeCount(changes) == 3; }, "Unstaging did not preserve the changed file and badge");
    runGit(L"reset --hard -q HEAD");
    waitFor([&] { return badge(0) && IsWindowVisible(changes) && gitChangeCount(changes) == 2; }, "Reverting changes did not remove the badge");
    fs::create_directory(fixture.root / L"nested 日本語");
    writeFile(fixture.root / L"nested 日本語" / L"added.txt");
    waitFor([&] { return badge(1) && IsWindowVisible(changes) && gitChangeCount(changes) == 4; }, "Creating an untracked file did not update Source Control");
    fs::remove(fixture.root / L"tracked.txt");
    waitFor([&] { return badge(2) && gitChangeCount(changes) == 5; }, "Deleting a tracked file did not add to the pending file count");
    writeLines(fixture.root / L"tracked.txt", 200);
    waitFor([&] { return badge(1) && gitChangeCount(changes) == 4; }, "Restoring a tracked file did not update the pending file count");
    clickAction(firstFile(false));
    waitFor([&] { return badge(1) && firstFile(true) && !firstFile(false); }, "The plus icon did not stage a nested untracked file");
    clickAction(firstFile(true));
    waitFor([&] { return badge(1) && !firstFile(true) && firstFile(false); }, "The minus icon did not preserve a nested untracked file");
    fs::rename(fixture.root / L"nested 日本語" / L"added.txt", fixture.root / L"moved.txt");
    waitFor([&] { return badge(1) && gitChangeCount(changes) == 3; }, "Renaming an untracked file did not update Source Control");
    fs::remove(fixture.root / L"moved.txt");
    waitFor([&] { return badge(0) && IsWindowVisible(changes) && gitChangeCount(changes) == 2; }, "Deleting the last untracked file did not clear Source Control");
}
void editableGitDiffTests(const fs::path& executable) {
    wchar_t git[MAX_PATH]{};
    const auto length = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, git, nullptr);
    if (!length || length >= MAX_PATH) { std::cout << "Editable Git diff tests skipped: Git for Windows is not on PATH.\n"; return; }
    Fixture fixture; const auto path = fixture.root / L"file.txt";
    const auto write = [&](const char* text) { std::ofstream file(path, std::ios::binary); file << text; check(file.good(), "Cannot write editable diff fixture"); };
    const auto runGit = [&](const std::wstring& args) {
        std::wstring command = L"\"" + std::wstring(git) + L"\" " + args;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        check(CreateProcessW(git, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, fixture.root.c_str(), &startup, &process) != FALSE, "Cannot start editable diff fixture Git");
        CloseHandle(process.hThread); const auto wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
        check(wait == WAIT_OBJECT_0 && !code, "Editable diff fixture Git failed");
    };
    write("base\r\n"); runGit(L"init -q"); runGit(L"config core.autocrlf false"); runGit(L"add file.txt");
    runGit(L"-c user.name=Test -c user.email=test@example.invalid commit -qm base");
    write("staged\r\n"); runGit(L"add file.txt"); write("working\r\n");
    Application app(executable, fixture.root); const auto tree = app.tree();
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr), changes = GetDlgItem(app.window, 4);
    waitFor([&] { return nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root)) != 0; }, "Editable fixture file did not load");
    const auto file = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root)); send(tree, treeSelect, file); waitLines(viewer, 1);
    const auto key = [&](WPARAM value, bool ctrl = false) {
        check(AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, TRUE) != FALSE, "Cannot attach editable diff input");
        BYTE previous[256]{}, keys[256]{}; GetKeyboardState(previous); if (ctrl) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        SetFocus(viewer); SetKeyboardState(keys); send(viewer, WM_KEYDOWN, value); SetKeyboardState(previous);
        AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, FALSE);
    };
    const auto text = [&] {
        key('A', true); key('C', true);
        if (!OpenClipboard(app.host)) return std::wstring{};
        const auto value = GetClipboardData(CF_UNICODETEXT); const auto data = value ? static_cast<const wchar_t*>(GlobalLock(value)) : nullptr;
        const std::wstring result = data ? data : L""; if (data) GlobalUnlock(value); CloseClipboard(); return result;
    };
    send(GetDlgItem(app.window, 3), BM_CLICK);
    waitFor([&] { return nextItem(changes, TreeRelation::Child, gitGroup(changes, false)) != 0; }, "Working Changes did not load");
    send(changes, treeSelect, nextItem(changes, TreeRelation::Child, gitGroup(changes, false)));
    waitFor([&] { return text() == L"working\r\n"; }, "Changes did not expose the editable working text");
    const UINT dpi = GetDpiForWindow(app.window);
    const auto switchLayout = [&](HWND pane, bool inlineView) {
        const auto tools = editorModeTools(app, pane, 136);
        click(app.window, tools.right - (tools.right - tools.left) * (inlineView ? 1 : 3) / 4, (tools.top + tools.bottom) / 2);
    };
    const auto inlineRows = scrollInfo(viewer).nMax;
    const auto tabCount = visibleTabCount(app);
    switchLayout(viewer, false);
    waitFor([&] { return scrollInfo(viewer).nMax < inlineRows; }, "Split in the tab tools did not change the diff layout");
    switchLayout(viewer, true);
    waitFor([&] { return scrollInfo(viewer).nMax == inlineRows; }, "Inline in the tab tools did not restore the diff layout");
    const auto pane = bounds(app.window, viewer);
    check(tabBounds(app, 0).right <= pane.right - MulDiv(136, dpi, 96) && visibleTabCount(app) == tabCount,
        "Diff tools overlapped a tab or opened/closed a tab");
    check(text() == L"working\r\n", "The tab tools changed the working diff text");
    send(viewer, WM_CHAR, '!'); check(readText(path) == "working\r\n", "Typing in Changes saved without Ctrl+S");
    send(tree, treeSelect, file); waitFor([&] { return text() == L"!"; }, "The ordinary tab lost unsaved diff edits");
    key(VK_END, true); send(viewer, WM_CHAR, 'n');
    send(changes, treeSelect, nextItem(changes, TreeRelation::Child, gitGroup(changes, false)));
    waitFor([&] { return text() == L"!n"; }, "Changes lost edits made in the ordinary tab");
    const auto group = gitGroup(changes, false); PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0);
    waitFor([&] { return gitGroup(changes, false) != group; }, "F5 did not refresh the editable comparison");
    check(text() == L"!n", "F5 overwrote unsaved diff text");
    key('Z', true); check(text() == L"!", "Changes and ordinary tabs did not share Undo history");
    key('S', true); waitFor([&] { return readText(path) == "!"; }, "Ctrl+S did not save the working file from Changes");
    send(changes, treeSelect, nextItem(changes, TreeRelation::Child, gitGroup(changes, true)));
    waitFor([&] { const auto current = text(); return current.find(L"base") != current.npos && current.find(L"staged") != current.npos; }, "Staged comparison lost the index content");
    send(viewer, WM_CHAR, '?'); key('S', true);
    check(readText(path) == "!", "Staged Changes accepted an edit or overwrote the working file");
}
void branchSidebarTests(const fs::path& executable) {
    wchar_t git[MAX_PATH]{};
    const auto length = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, git, nullptr);
    if (!length || length >= MAX_PATH) { std::cout << "Branch tests skipped: Git for Windows is not on PATH.\n"; return; }
    Fixture fixture;
    const auto runGit = [&](const std::wstring& args) {
        std::wstring command = L"\"" + std::wstring(git) + L"\" " + args;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        check(CreateProcessW(git, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, fixture.root.c_str(), &startup, &process) != FALSE, "Cannot start fixture Git");
        CloseHandle(process.hThread); const auto wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
        check(wait == WAIT_OBJECT_0 && code == 0, "Branch fixture Git command failed");
    };
    const auto write = [&](const fs::path& name, const char* text) { std::ofstream file(fixture.root / name, std::ios::binary); file << text; check(!file.fail(), "Cannot write branch fixture"); };
    const auto children = [](HWND tree, TreeItem parent) {
        size_t count = 0;
        for (auto item = nextItem(tree, TreeRelation::Child, parent); item; item = nextItem(tree, TreeRelation::Next, item)) ++count;
        return count;
    };
    runGit(L"init --quiet --template= --initial-branch=main");
    runGit(L"config user.name PoppyTest"); runGit(L"config user.email poppy@example.invalid");
    runGit(L"config commit.gpgsign false"); runGit(L"config core.autocrlf false");
    fs::create_directories(fixture.root / L"src");
    write(L"src/file.txt", "initial\n"); write(L"deleted.txt", "deleted\n");
    runGit(L"add ."); runGit(L"commit -qm root");
    for (int i = 0; i < 50; ++i) runGit(L"commit --allow-empty -qm history");
    write(L"src/file.txt", "committed\n"); fs::remove(fixture.root / L"deleted.txt");
    runGit(L"add -A"); runGit(L"commit -qm latest");
    runGit(L"branch alternate"); runGit(L"update-ref refs/remotes/origin/remote HEAD");
    write(L"src/file.txt", "staged\n"); runGit(L"add .");
    write(L"src/file.txt", "working\n"); write(L"untracked.txt", "untracked\n");
    {
        Application app(executable, fixture.root); app.tree();
        const auto changes = GetDlgItem(app.window, 4), branches = changes;
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        send(GetDlgItem(app.window, 3), BM_CLICK);
        waitFor([&] { return IsWindowVisible(changes) && gitGroup(changes, false); }, "Source Control did not load the current branch");
        const auto pane = bounds(app.window, changes);
        click(app.window, pane.left + (pane.right - pane.left) * 3 / 4, pane.top - MulDiv(14, GetDpiForWindow(app.window), 96));
        waitFor([&] { return gitChangeCount(changes) == 5; }, "Source Control did not separate partially staged changes");
        check(!GetDlgItem(app.window, 5) && !GetDlgItem(app.window, 6), "The separate Branch button or pane still exists");
        const auto staged = gitGroup(changes, true), working = nextItem(changes, TreeRelation::Next, staged);
        check(children(changes, staged) == 1 && children(changes, working) == 2, "Staged and unstaged groups contain the wrong files");
        const auto clickFileAction = [&](TreeItem item, int offset) {
            RECT client{}; GetClientRect(changes, &client);
            send(changes, treeSelect, item, 0);
            const int height = static_cast<int>(send(changes, treeGetRowHeight));
            const int y = (static_cast<int>(send(changes, treeGetRow, item)) - scrollInfo(changes).nPos) * height + height / 2;
            const int x = client.right - MulDiv(offset, GetDpiForWindow(app.window), 96);
            send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(x, y)); click(changes, x, y); send(changes, WM_MOUSELEAVE);
        };
        const auto checkEditable = [&] {
            // A diff can briefly have the same row count while the file loads.
            waitFor([&] {
                if (lastTextRow(viewer) == 2) return true;
                send(viewer, WM_CHAR, '\r'); return false;
            }, "Open File did not open an editable working file");
            send(viewer, WM_KEYDOWN, VK_BACK); waitLines(viewer, 1);
        };
        const auto viewerKey = [&](WPARAM value, bool ctrl = false) {
            check(AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, TRUE) != FALSE, "Cannot attach history file input");
            BYTE previous[256]{}, keys[256]{}; GetKeyboardState(previous);
            if (ctrl) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
            SetFocus(viewer); SetKeyboardState(keys); send(viewer, WM_KEYDOWN, value); SetKeyboardState(previous);
            AttachThreadInput(GetCurrentThreadId(), app.process.dwThreadId, FALSE);
        };
        const auto viewerText = [&] {
            viewerKey('A', true); viewerKey('C', true);
            if (!OpenClipboard(app.host)) return std::wstring{};
            const auto value = GetClipboardData(CF_UNICODETEXT); const auto data = value ? static_cast<const wchar_t*>(GlobalLock(value)) : nullptr;
            const std::wstring result = data ? data : L""; if (data) GlobalUnlock(value); CloseClipboard(); return result;
        };
        // File actions read left to right: Open File, Discard Changes, Stage.
        const auto workingFile = nextItem(changes, TreeRelation::Child, working);
        clickFileAction(workingFile, 74); waitLines(viewer, 1);
        checkEditable();
        // Staged rows open the working file beside Unstage, without discarding.
        clickFileAction(nextItem(changes, TreeRelation::Child, staged), 46); waitLines(viewer, 1);
        checkEditable();
        discardItem(app, changes, nextItem(changes, TreeRelation::Child, gitGroup(changes, false)), IDCANCEL, false);
        check(readText(fixture.root / L"src/file.txt") == "working\n" && readText(fixture.root / L"untracked.txt") == "untracked\n",
              "Cancelling single-file discard changed working files");
        discardItem(app, changes, nextItem(changes, TreeRelation::Child, gitGroup(changes, false)), IDOK, false);
        waitFor([&] { return children(changes, gitGroup(changes, false)) == 1; }, "Single-file discard did not update Changes");
        check(readText(fixture.root / L"src/file.txt") == "staged\n" && readText(fixture.root / L"untracked.txt") == "untracked\n",
              "Single-file discard lost staged text or changed another file");
        // Restore the fixture for the existing group-action tests.
        write(L"src/file.txt", "working\n");
        waitFor([&] { return children(changes, gitGroup(changes, false)) == 2; }, "Single-file discard fixture did not refresh");
        const auto refreshedStaged = gitGroup(changes, true), refreshedWorking = gitGroup(changes, false);
        send(changes, treeSelect, refreshedStaged); send(changes, WM_KEYDOWN, VK_F4); waitLines(viewer, 1, true);
        const auto rect = bounds(app.window, changes);
        const int height = static_cast<int>(send(changes, treeGetRowHeight));
        const int row = static_cast<int>(send(changes, treeGetRow, refreshedWorking));
        click(changes, rect.right - rect.left - MulDiv(18, GetDpiForWindow(app.window), 96), (row - scrollInfo(changes).nPos) * height + height / 2);
        waitLines(viewer, 4, true);
        // The group keeps its diff action and adds Stage All Changes beside it,
        // including when its file list is collapsed.
        expand(changes, refreshedWorking, false);
        const int stageX = rect.right - rect.left - MulDiv(46, GetDpiForWindow(app.window), 96);
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(stageX, (row - scrollInfo(changes).nPos) * height + height / 2));
        click(changes, stageX, (row - scrollInfo(changes).nPos) * height + height / 2);
        send(changes, WM_MOUSELEAVE);
        waitFor([&] {
            const auto first = gitGroup(changes, true), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 2 && children(changes, second) == 0;
        }, "Stage All Changes did not stage partially staged and untracked files from a collapsed group");
        waitLines(viewer, 0);
        runGit(L"diff --quiet");
        const auto stagedAll = gitGroup(changes, true);
        const int stagedRow = static_cast<int>(send(changes, treeGetRow, stagedAll));
        const int stagedY = (stagedRow - scrollInfo(changes).nPos) * height + height / 2;
        // Open All Changes remains the rightmost action on Staged Changes.
        click(changes, rect.right - rect.left - MulDiv(18, GetDpiForWindow(app.window), 96), stagedY);
        waitLines(viewer, 4, true);
        expand(changes, stagedAll, false);
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(stageX, stagedY));
        click(changes, stageX, stagedY); send(changes, WM_MOUSELEAVE);
        waitFor([&] {
            const auto first = gitGroup(changes, true), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 0 && children(changes, second) == 2;
        }, "Unstage All Changes did not clear the index from a collapsed Staged Changes group");
        waitLines(viewer, 0);
        runGit(L"diff --cached --quiet");
        wchar_t badge[128]{}; GetWindowTextW(GetDlgItem(app.window, 3), badge, 128);
        check(std::wstring(badge) == L"Source Control (2)", "Unstage All Changes lost or duplicated the change count");
        runGit(L"add -- src/file.txt");
        write(L"src/file.txt", "discard this\n");
        write(L".git/info/exclude", "ignored.txt\n"); write(L"ignored.txt", "keep\n");
        waitFor([&] {
            const auto first = gitGroup(changes, true), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 1 && children(changes, second) == 2;
        }, "Discard fixture did not show partially staged and untracked changes");
        auto discardWorking = gitGroup(changes, false);
        send(changes, treeSelect, discardWorking); send(changes, WM_KEYDOWN, VK_F4); waitLines(viewer, 4, true);
        discardGroup(app, changes, discardWorking, IDCANCEL);
        check(fs::exists(fixture.root / L"untracked.txt") && fs::file_size(fixture.root / L"src/file.txt") == 13,
              "Cancelling discard changed working files");
        waitLines(viewer, 4, true);
        discardWorking = gitGroup(changes, false);
        discardGroup(app, changes, discardWorking, IDOK);
        waitFor([&] {
            const auto first = gitGroup(changes, true), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 1 && children(changes, second) == 0 && !fs::exists(fixture.root / L"untracked.txt");
        }, "Discard did not preserve Staged Changes or clear Changes");
        waitLines(viewer, 0);
        runGit(L"diff --quiet");
        check(fs::file_size(fixture.root / L"src/file.txt") == 8 && fs::exists(fixture.root / L"ignored.txt"),
              "Discard did not restore the staged text or removed ignored files");
        GetWindowTextW(GetDlgItem(app.window, 3), badge, 128);
        check(std::wstring(badge) == L"Source Control (1)", "Discard did not update the badge");
        write(L"src/file.txt", "stash working\n"); write(L"stash 日本語.txt", "stash untracked\n");
        waitFor([&] {
            const auto first = gitGroup(changes, true), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 1 && children(changes, second) == 2;
        }, "Stash fixture did not show partially staged and untracked changes");
        const auto stashWorking = gitGroup(changes, false);
        send(changes, treeSelect, stashWorking); send(changes, WM_KEYDOWN, VK_F4); waitLines(viewer, 4, true);
        expand(changes, stashWorking, false);
        // Stash is the fourth action, to the left of discard, stage and diff.
        const int stashX = rect.right - rect.left - MulDiv(102, GetDpiForWindow(app.window), 96);
        const int stashY = static_cast<int>(send(changes, treeGetRow, stashWorking)) * height + height / 2;
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(stashX, stashY)); click(changes, stashX, stashY); send(changes, WM_MOUSELEAVE);
        waitFor([&] {
            const auto first = gitGroup(changes, true), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 1 && children(changes, second) == 0 && !fs::exists(fixture.root / L"stash 日本語.txt");
        }, "Stash Unstaged Changes did not preserve Staged Changes or clear a collapsed Changes group");
        waitLines(viewer, 0);
        runGit(L"diff --quiet"); runGit(L"rev-parse --verify refs/stash"); runGit(L"diff --cached --quiet stash@{0}^2 --");
        check(fs::file_size(fixture.root / L"src/file.txt") == 8 && fs::exists(fixture.root / L"ignored.txt"),
              "Stash did not restore the staged text or removed ignored files");
        GetWindowTextW(GetDlgItem(app.window, 3), badge, 128);
        check(std::wstring(badge) == L"Source Control (1)", "Stash did not update the badge");
        write(L"keep untracked.txt", "keep\n");
        waitFor([&] {
            const auto first = gitGroup(changes, true), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 1 && children(changes, second) == 1;
        }, "Staged stash fixture did not show staged and untracked changes");
        const auto stashStaged = gitGroup(changes, true);
        // Diff and Unstage All Changes retain their positions beside stash.
        click(changes, rect.right - rect.left - MulDiv(18, GetDpiForWindow(app.window), 96),
              (static_cast<int>(send(changes, treeGetRow, stashStaged)) - scrollInfo(changes).nPos) * height + height / 2);
        waitLines(viewer, 1, true);
        expand(changes, stashStaged, false);
        const int stagedStashX = rect.right - rect.left - MulDiv(74, GetDpiForWindow(app.window), 96);
        const int stagedStashY = (static_cast<int>(send(changes, treeGetRow, stashStaged)) - scrollInfo(changes).nPos) * height + height / 2;
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(stagedStashX, stagedStashY));
        click(changes, stagedStashX, stagedStashY); send(changes, WM_MOUSELEAVE);
        waitFor([&] {
            const auto first = gitGroup(changes, true), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 0 && children(changes, second) == 1;
        }, "Stash Staged Changes did not clear a collapsed Staged Changes group or kept the wrong Changes");
        waitLines(viewer, 0);
        runGit(L"diff --cached --quiet"); runGit(L"diff --quiet"); runGit(L"diff --quiet stash@{0}^2 stash@{0} --");
        check(fs::file_size(fixture.root / L"src/file.txt") == 10 && fs::exists(fixture.root / L"keep untracked.txt") &&
              fs::exists(fixture.root / L"ignored.txt"), "Staged stash did not restore HEAD content or lost untracked/ignored files");
        GetWindowTextW(GetDlgItem(app.window, 3), badge, 128);
        check(std::wstring(badge) == L"Source Control (1)", "Staged stash did not update the badge");
        waitFor([&] { return IsWindowVisible(branches) && children(branches, gitBranch(branches)) == 53; }, "Branch did not load 50 commits and a load-more row");
        check(IsWindowVisible(changes), "Source Control hid its combined branch and change list");
        check(children(branches, 0) == 4, "Source Control did not list local and remote branches");
        const auto branch = gitBranch(branches), latest = nextItem(branches, TreeRelation::Next, gitGroup(branches, false));
        check(nextItem(branches, TreeRelation::Parent, gitGroup(branches, true)) == branch, "Working changes are not inside the current branch");
        auto more = latest;
        for (int i = 0; i < 50; ++i) more = nextItem(branches, TreeRelation::Next, more);
        check(more != 0, "The 51st row is missing");
        send(branches, treeSelect, more);
        waitFor([&] { return children(branches, branch) == 54; }, "Clicking the 51st row did not append the remaining commits");
        auto rootCommit = latest;
        while (const auto next = nextItem(branches, TreeRelation::Next, rootCommit)) rootCommit = next;
        click(app.window, pane.left + (pane.right - pane.left) / 4, pane.top - MulDiv(14, GetDpiForWindow(app.window), 96));
        send(branches, treeSelect, latest);
        waitFor([&] { return children(branches, latest) == 2; }, "Opening a commit did not build its changed-file tree");
        const auto directory = nextItem(branches, TreeRelation::Child, latest), file = nextItem(branches, TreeRelation::Child, directory);
        check(file != 0, "Commit files are missing their folder hierarchy");
        send(branches, treeSelect, file); waitLines(viewer, 1);
        clickFileAction(file, 18); checkEditable();
        click(app.window, pane.left + (pane.right - pane.left) * 3 / 4, pane.top - MulDiv(14, GetDpiForWindow(app.window), 96));
        check(children(branches, latest) == 2 && !nextItem(branches, TreeRelation::Child, nextItem(branches, TreeRelation::Child, latest)), "List did not flatten loaded commit files");
        send(branches, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 1);
        send(branches, WM_KEYDOWN, VK_F4); checkEditable();
        click(app.window, pane.left + (pane.right - pane.left) / 4, pane.top - MulDiv(14, GetDpiForWindow(app.window), 96));
        check(children(branches, nextItem(branches, TreeRelation::Child, latest)) == 1, "Tree did not restore loaded commit folders");
        send(branches, treeSelect, latest); send(branches, WM_KEYDOWN, VK_F4); waitLines(viewer, 4, true);
        const auto previews = visibleTabCount(app);
        send(branches, treeSelect, rootCommit); send(branches, WM_KEYDOWN, VK_F4); waitLines(viewer, 5, true);
        check(visibleTabCount(app) == previews, "Browsing All Changes for different commits accumulated tabs");
        waitFor([&] { return children(branches, rootCommit) == 2; }, "The oldest commit did not load its files");
        const auto oldFile = nextItem(branches, TreeRelation::Child, nextItem(branches, TreeRelation::Child, rootCommit));
        check(oldFile != 0, "The oldest commit is missing its nested file");
        // Open Revision sits immediately to the left of Open File.
        clickFileAction(oldFile, 46);
        waitFor([&] { return viewerText() == L"initial\n"; }, "Open Revision did not display the selected commit's full file");
        send(viewer, WM_CHAR, '!');
        check(viewerText() == L"initial\n" && readText(fixture.root / L"src/file.txt") == "committed\n",
            "Open Revision allowed editing or changed the current working file");
        clickFileAction(oldFile, 18);
        waitFor([&] { return viewerText() == L"committed\n"; }, "History Open File opened the old revision instead of the current working file");
        viewerKey(VK_END, true); send(viewer, WM_CHAR, '!');
        send(branches, treeSelect, oldFile); waitLines(viewer, 1);
        send(branches, WM_KEYDOWN, VK_F4);
        waitFor([&] { return viewerText() == L"committed\n!"; }, "History Open File lost unsaved edits in the existing file tab");
        check(readText(fixture.root / L"src/file.txt") == "committed\n", "History Open File saved edits without Ctrl+S");
        viewerKey('Z', true);
        waitFor([&] { return viewerText() == L"committed\n"; }, "History Open File lost the working file's Undo history");
        send(branches, treeSelect, rootCommit); send(branches, WM_KEYDOWN, VK_F4); waitLines(viewer, 5, true);
        const auto oldWorking = gitGroup(branches, false);
        keepTab(app, visibleTabCount(app) - 1);
        PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0);
        waitFor([&] { return gitGroup(branches, false) != oldWorking; }, "Source Control did not refresh working changes");
        check(children(branches, branch) == 54 && children(branches, latest) == 2, "Refreshing working changes discarded loaded history");
        runGit(L"switch --quiet alternate");
        waitFor([&] {
            const auto current = gitBranch(branches);
            return current != branch && gitGroup(branches, false) && children(branches, branch) == 52;
        }, "Switching branches did not move both working groups off the previous branch");
        const auto alternate = gitBranch(branches);
        runGit(L"checkout --detach --quiet");
        waitFor([&] {
            const auto current = gitBranch(branches);
            return current != alternate && children(branches, 0) == 5 && gitGroup(branches, false) &&
                nextItem(branches, TreeRelation::Parent, gitGroup(branches, true)) == current;
        }, "Detached HEAD did not get working groups above its history");
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto branches = GetDlgItem(app.window, 4), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitFor([&] { return IsWindowVisible(branches); }, "Restart did not restore Source Control");
        waitLines(viewer, 5, true);
    }
}
void comparisonSidebarTests(const fs::path& executable) {
    wchar_t git[MAX_PATH]{};
    const auto length = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, git, nullptr);
    if (!length || length >= MAX_PATH) { std::cout << "Comparison GUI tests skipped: Git for Windows is not on PATH.\n"; return; }
    Fixture fixture;
    const auto runGit = [&](const std::wstring& args) {
        std::wstring command = L"\"" + std::wstring(git) + L"\" " + args;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        check(CreateProcessW(git, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, fixture.root.c_str(), &startup, &process) != FALSE, "Cannot start comparison fixture Git");
        CloseHandle(process.hThread); const auto wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
        check(wait == WAIT_OBJECT_0 && code == 0, "Comparison fixture Git failed");
    };
    runGit(L"init --quiet --template= --initial-branch=main");
    runGit(L"config user.name PoppyTest"); runGit(L"config user.email poppy@example.invalid");
    runGit(L"config commit.gpgsign false"); runGit(L"config tag.gpgsign false"); runGit(L"config core.autocrlf false");
    writeLines(fixture.root / L"base.txt", 1); runGit(L"add ."); runGit(L"commit -qm base"); runGit(L"tag -a v1.0 -m release");
    runGit(L"switch -qc topic"); writeLines(fixture.root / L"topic.txt", 300); runGit(L"add ."); runGit(L"commit -qm topic");
    runGit(L"switch -q main"); writeLines(fixture.root / L"main.txt", 100); runGit(L"add ."); runGit(L"commit -qm main");
    auto commitId = readText(fixture.root / L".git" / L"refs" / L"heads" / L"main"); commitId.resize(12);
    Application app(executable, fixture.root); app.tree();
    const auto changes = GetDlgItem(app.window, 4), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    const auto explorer = app.tree();
    waitFor([&] { return send(explorer, treeGetCount) == 4; }, "Comparison fixture files did not load");
    // The .git directory sorts before base.txt and main.txt.
    const auto gitDirectory = nextItem(explorer, TreeRelation::Child, nextItem(explorer, TreeRelation::Root));
    send(explorer, treeSelect, nextItem(explorer, TreeRelation::Next, gitDirectory));
    waitLines(viewer, 1); keepTab(app, 0);
    send(GetDlgItem(app.window, 3), BM_CLICK);
    waitFor([&] { return gitGroup(changes, true) != 0; }, "Comparison fixture branches did not load");
    const auto compare = nextItem(changes, TreeRelation::Root), current = gitBranch(changes), topic = nextItem(changes, TreeRelation::Next, current);
    check(compare && current && topic && !nextItem(changes, TreeRelation::Next, topic) &&
          !nextItem(changes, TreeRelation::Parent, compare) && nextItem(changes, TreeRelation::Child, current) == gitGroup(changes, true),
          "Compare is not a single repository-level row outside the branches");
    const auto picker = FindWindowExW(app.window, nullptr, L"PoppyQuickPick", nullptr);
    const auto pickerTitle = [&] { wchar_t caption[1024]{}; GetWindowTextW(picker, caption, 1024); return std::wstring(caption); };
    const auto start = [&] {
        // Exercise the label click, independently of the expand/collapse arrow.
        send(changes, WM_VSCROLL, SB_TOP);
        const int height = static_cast<int>(send(changes, treeGetRowHeight));
        const int y = (static_cast<int>(send(changes, treeGetRow, compare)) - scrollInfo(changes).nPos) * height + height / 2;
        click(changes, MulDiv(70, GetDpiForWindow(app.window), 96), y);
        check(picker && IsWindowVisible(picker) && pickerTitle() == L"Compare: Select source", "Compare did not ask for the source first");
    };
    const auto pick = [&](const std::wstring& query, bool source) {
        for (const auto ch : query) send(picker, WM_CHAR, ch);
        waitFor([&] {
            send(picker, WM_KEYDOWN, VK_RETURN);
            return source ? IsWindowVisible(picker) && pickerTitle().find(L"Compare: Select target") == 0 : !IsWindowVisible(picker);
        }, "The comparison picker did not advance to its next step");
    };
    const auto choose = [&](const std::wstring& source, const std::wstring& target) {
        start(); pick(source, true); pick(target, false);
    };
    const auto action = [&](int offset) {
        send(changes, WM_VSCROLL, SB_TOP);
        RECT client{}; GetClientRect(changes, &client);
        const int height = static_cast<int>(send(changes, treeGetRowHeight));
        const int y = (static_cast<int>(send(changes, treeGetRow, compare)) - scrollInfo(changes).nPos) * height + height / 2;
        click(changes, client.right - MulDiv(offset, GetDpiForWindow(app.window), 96), y);
    };
    choose(L"main", L"topic"); waitLines(viewer, 301, true);
    check(!fs::exists(fixture.root / L"topic.txt"), "Comparing a noncurrent branch checked it out");
    const auto swap = nextItem(changes, TreeRelation::Child, compare);
    send(changes, treeSelect, swap); waitLines(viewer, 101, true);
    auto mode = nextItem(changes, TreeRelation::Next, nextItem(changes, TreeRelation::Child, compare));
    send(changes, treeSelect, mode); waitLines(viewer, 402, true);
    // The newly allocated swap row reverses literal before/after revisions.
    send(changes, treeSelect, nextItem(changes, TreeRelation::Child, compare)); waitLines(viewer, 402, true);
    choose(L"v1.0", L"topic"); waitLines(viewer, 301, true);
    choose(std::wstring(commitId.begin(), commitId.end()), L"topic"); waitLines(viewer, 402, true);
    // Individual comparison files use committed contents and remain read-only.
    mode = nextItem(changes, TreeRelation::Next, nextItem(changes, TreeRelation::Child, compare));
    const auto file = nextItem(changes, TreeRelation::Next, mode);
    send(changes, treeSelect, file); waitLines(viewer, 100);
    send(viewer, WM_CHAR, '!');
    check(readText(fixture.root / L"main.txt").find('!') == std::string::npos, "A comparison edited a working file");
    runGit(L"diff --quiet"); runGit(L"diff --cached --quiet");
    // Escape cancels a new selection without replacing the existing review.
    const auto previous = nextItem(changes, TreeRelation::Child, compare);
    start(); send(picker, WM_KEYDOWN, VK_ESCAPE);
    check(!IsWindowVisible(picker), "Escape did not close the comparison picker"); waitLines(viewer, 100);
    start(); pick(L"v1.0", true); send(picker, WM_KEYDOWN, VK_ESCAPE);
    check(!IsWindowVisible(picker) && nextItem(changes, TreeRelation::Child, compare) == previous,
          "Cancelling target selection replaced the previous comparison"); waitLines(viewer, 100);
    auto oldGroup = gitGroup(changes, false); send(app.window, WM_KEYDOWN, VK_F5);
    waitFor([&] { return gitGroup(changes, false) != oldGroup; }, "The comparison fixture did not refresh");
    check(nextItem(changes, TreeRelation::Root) == compare && nextItem(changes, TreeRelation::Child, compare) == previous,
          "Refreshing the repository discarded or duplicated the comparison"); waitLines(viewer, 100);
    // Open All Changes is left of the clear button and remains the F4 action.
    action(46); waitLines(viewer, 402, true);
    check(!IsWindowVisible(picker) && nextItem(changes, TreeRelation::Child, compare) == previous, "Open All Changes cleared the comparison or opened a picker");
    toggleTabPin(app, visibleTabCount(app) - 1);
    send(changes, treeSelect, file); waitLines(viewer, 100);
    action(18);
    check(!nextItem(changes, TreeRelation::Child, compare) && !expanded(changes, compare), "The rightmost clear button did not reset Compare");
    waitFor([&] { return visibleTabCount(app) == 1; }, "Clear left comparison tabs open or closed an unrelated file"); waitLines(viewer, 1);
    // The same Compare row survives refresh, with no completed request restoring it.
    oldGroup = gitGroup(changes, false); send(app.window, WM_KEYDOWN, VK_F5);
    waitFor([&] { return gitGroup(changes, false) != oldGroup; }, "The cleared repository did not refresh");
    check(nextItem(changes, TreeRelation::Root) == compare && !nextItem(changes, TreeRelation::Child, compare), "Refreshing duplicated or restored the cleared comparison");
    // Arbitrary commits/tags work as both endpoints after resetting the mode.
    choose(L"v1.0", std::wstring(commitId.begin(), commitId.end())); waitLines(viewer, 101, true);
    choose(L"main", L"topic"); action(18);
    oldGroup = gitGroup(changes, false); send(app.window, WM_KEYDOWN, VK_F5);
    waitFor([&] { return gitGroup(changes, false) != oldGroup; }, "The pending comparison did not finish before refresh");
    check(!nextItem(changes, TreeRelation::Child, compare) && visibleTabCount(app) == 1, "A late comparison completion restored content after Clear"); waitLines(viewer, 1);
    runGit(L"diff --quiet"); runGit(L"diff --cached --quiet");
}
void multiRepositoryTests(const fs::path& executable) {
    wchar_t git[MAX_PATH]{};
    const auto length = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, git, nullptr);
    if (!length || length >= MAX_PATH) { std::cout << "Multi-repository GUI tests skipped: Git for Windows is not on PATH.\n"; return; }
    Fixture fixture;
    const auto workspace = fixture.root / L"app";
    fs::create_directory(workspace);
    const auto client = workspace / L"app-client", server = workspace / L"app-server";
    fs::create_directory(client); fs::create_directory(server);
    const auto runGit = [&](const fs::path& directory, const std::wstring& args, DWORD expected = 0) {
        std::wstring command = L"\"" + std::wstring(git) + L"\" " + args;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        check(CreateProcessW(git, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process) != FALSE, "Cannot start multi-repository Git fixture");
        CloseHandle(process.hThread); const auto wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
        check(wait == WAIT_OBJECT_0 && code == expected, "Multi-repository Git fixture command failed");
    };
    // Match a multi-repository folder inside an unrelated ancestor worktree.
    runGit(fixture.root, L"init --quiet --template= --initial-branch=main");
    writeLines(fixture.root / L"unrelated.txt", 1);
    for (const auto& directory : {client, server}) {
        runGit(directory, L"init --quiet --template= --initial-branch=main");
        runGit(directory, L"config user.name PoppyTest"); runGit(directory, L"config user.email poppy@example.invalid");
        runGit(directory, L"config commit.gpgsign false"); runGit(directory, L"config core.autocrlf false");
        writeLines(directory / L"extra.txt", 1); writeLines(directory / L"same.txt", directory == client ? 1 : 3);
        runGit(directory, L"add ."); runGit(directory, L"commit -qm initial");
        runGit(directory, L"commit --allow-empty -qm latest");
        writeLines(directory / L"same.txt", directory == client ? 2 : 4);
        writeLines(directory / L"untracked.txt", directory == client ? 111 : 222);
    }
    const auto children = [](HWND tree, TreeItem parent) {
        size_t count = 0;
        for (auto item = nextItem(tree, TreeRelation::Child, parent); item; item = nextItem(tree, TreeRelation::Next, item)) ++count;
        return count;
    };
    {
        Application app(executable, workspace); app.tree();
        const auto changes = GetDlgItem(app.window, 4), branches = changes;
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        send(GetDlgItem(app.window, 3), BM_CLICK);
        const auto roots = [&](HWND tree) { return children(tree, 0) == 2; };
        waitFor([&] { return IsWindowVisible(changes) && roots(changes); }, "Source Control did not discover sibling repositories");
        wchar_t title[128]{}; GetWindowTextW(GetDlgItem(app.window, 3), title, 128);
        check(std::wstring(title) == L"Source Control (4)", "The badge counted changes outside the opened folder");
        const auto group = [&](TreeItem repo, bool staged) { return gitGroup(changes, staged, repo); };
        auto clientRepo = nextItem(changes, TreeRelation::Root), serverRepo = nextItem(changes, TreeRelation::Next, clientRepo);
        waitFor([&] { return group(clientRepo, false) && group(serverRepo, false); }, "Repositories did not get independent change groups inside their current branches");
        for (const auto repo : {clientRepo, serverRepo}) {
            const auto tracked = nextItem(changes, TreeRelation::Child, group(repo, false));
            send(changes, treeSelect, nextItem(changes, TreeRelation::Next, tracked));
            waitLines(viewer, repo == clientRepo ? 111 : 222);
        }
        send(changes, treeSelect, nextItem(changes, TreeRelation::Child, group(clientRepo, false)));
        send(changes, WM_KEYDOWN, VK_F4);
        waitFor([&] { clientRepo = nextItem(changes, TreeRelation::Root); return children(changes, group(clientRepo, true)) == 1; }, "Staging did not update the owning repository");
        runGit(client, L"diff --cached --quiet", 1); runGit(server, L"diff --cached --quiet");
        send(changes, treeSelect, nextItem(changes, TreeRelation::Child, group(clientRepo, true)));
        send(changes, WM_KEYDOWN, VK_F4);
        waitFor([&] { clientRepo = nextItem(changes, TreeRelation::Root); return children(changes, group(clientRepo, true)) == 0; }, "Unstaging did not update the owning repository");
        runGit(client, L"diff --cached --quiet");
        RECT changesRect{}; GetClientRect(changes, &changesRect);
        const int height = static_cast<int>(send(changes, treeGetRowHeight));
        const int row = static_cast<int>(send(changes, treeGetRow, group(clientRepo, false)));
        const int x = changesRect.right - MulDiv(46, GetDpiForWindow(app.window), 96), y = (row - scrollInfo(changes).nPos) * height + height / 2;
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(x, y)); click(changes, x, y); send(changes, WM_MOUSELEAVE);
        waitFor([&] {
            clientRepo = nextItem(changes, TreeRelation::Root); serverRepo = nextItem(changes, TreeRelation::Next, clientRepo);
            return children(changes, group(clientRepo, true)) == 2 && children(changes, group(clientRepo, false)) == 0 &&
                children(changes, group(serverRepo, true)) == 0 && children(changes, group(serverRepo, false)) == 2;
        }, "Stage All Changes affected a sibling repository or missed a file in the selected repository");
        runGit(client, L"diff --cached --quiet", 1); runGit(server, L"diff --cached --quiet");
        send(changes, treeSelect, nextItem(changes, TreeRelation::Child, group(serverRepo, false)));
        send(changes, WM_KEYDOWN, VK_F4);
        waitFor([&] {
            clientRepo = nextItem(changes, TreeRelation::Root); serverRepo = nextItem(changes, TreeRelation::Next, clientRepo);
            return children(changes, group(serverRepo, true)) == 1;
        }, "Could not stage the sibling repository before testing Unstage All Changes");
        const auto stagedClient = group(clientRepo, true);
        expand(changes, stagedClient, false);
        const int unstageRow = static_cast<int>(send(changes, treeGetRow, stagedClient));
        const int unstageY = (unstageRow - scrollInfo(changes).nPos) * height + height / 2;
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(x, unstageY)); click(changes, x, unstageY); send(changes, WM_MOUSELEAVE);
        waitFor([&] {
            clientRepo = nextItem(changes, TreeRelation::Root); serverRepo = nextItem(changes, TreeRelation::Next, clientRepo);
            return children(changes, group(clientRepo, true)) == 0 && children(changes, group(clientRepo, false)) == 2 &&
                children(changes, group(serverRepo, true)) == 1 && children(changes, group(serverRepo, false)) == 1;
        }, "Unstage All Changes affected a sibling repository or left a file staged in the selected repository");
        runGit(client, L"diff --cached --quiet"); runGit(server, L"diff --cached --quiet", 1);
        runGit(fixture.root, L"diff --cached --quiet");
        discardGroup(app, changes, group(clientRepo, false), IDOK);
        waitFor([&] {
            clientRepo = nextItem(changes, TreeRelation::Root); serverRepo = nextItem(changes, TreeRelation::Next, clientRepo);
            return children(changes, group(clientRepo, false)) == 0 && children(changes, group(serverRepo, true)) == 1 &&
                children(changes, group(serverRepo, false)) == 1;
        }, "Discard All Changes affected a sibling repository or retained Changes");
        runGit(client, L"diff --quiet"); runGit(server, L"diff --cached --quiet", 1);
        waitFor([&] { return IsWindowVisible(branches) && roots(branches); }, "Branch did not discover sibling repositories");
        const auto clientBranchRepo = nextItem(branches, TreeRelation::Root), serverBranchRepo = nextItem(branches, TreeRelation::Next, clientBranchRepo);
        for (const auto repo : {clientBranchRepo, serverBranchRepo}) {
            const auto branch = gitBranch(branches, repo);
            waitFor([&] { return children(branches, branch) == 4; }, "A repository's main branch did not load its history");
            const auto latest = nextItem(branches, TreeRelation::Next, gitGroup(branches, false, repo)), commit = nextItem(branches, TreeRelation::Next, latest);
            expand(branches, commit);
            waitFor([&] { return children(branches, commit) == 2; }, "A repository's commit did not load its files");
            const auto extra = nextItem(branches, TreeRelation::Child, commit);
            send(branches, treeSelect, nextItem(branches, TreeRelation::Next, extra));
            waitLines(viewer, repo == clientBranchRepo ? 1 : 3);
        }
    }
    {
        Application app(executable, workspace); app.tree();
        waitLines(FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr), 3);
    }
}
std::set<DWORD> childProcesses(DWORD parent) {
    std::set<DWORD> result; const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    check(snapshot != INVALID_HANDLE_VALUE, "Cannot inspect shell processes");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do { if (entry.th32ParentProcessID == parent && (_wcsicmp(entry.szExeFile, L"powershell.exe") == 0 || _wcsicmp(entry.szExeFile, L"bash.exe") == 0)) result.insert(entry.th32ProcessID); } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot); return result;
}
void multiProjectTests(const fs::path& executable) {
    Fixture a, b, c;
    writeLines(a.root / L"a.txt", 200); writeLines(b.root / L"b.txt", 300); writeLines(c.root / L"c.txt", 400);
    {
    Application app(executable, a.root);
    const auto treeA = app.tree(); const auto surfaceA = app.window;
    const auto viewerA = FindWindowExW(surfaceA, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return nextItem(treeA, TreeRelation::Child, nextItem(treeA, TreeRelation::Root)) != 0; }, "Project A did not load");
    send(treeA, treeSelect, nextItem(treeA, TreeRelation::Child, nextItem(treeA, TreeRelation::Root))); waitLines(viewerA, 200); keepTab(app, 0);
    send(viewerA, WM_VSCROLL, SB_PAGEDOWN); const int top = scrollInfo(viewerA).nPos;
    const auto rootA = nextItem(treeA, TreeRelation::Root);
    const auto open = [&](const fs::path& path) {
        const auto text = path.native(); COPYDATASTRUCT data{0x50505901, static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)), const_cast<wchar_t*>(text.c_str())};
        check(send(app.host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)), "Could not add a second project");
        app.tree();
    };
    const auto select = [&](size_t index) { const UINT dpi=GetDpiForWindow(app.host);click(app.host,MulDiv(60,dpi,96),MulDiv(44+static_cast<int>(index)*projectRowHeight+24,dpi,96));app.tree(); };
    open(b.root); const auto surfaceB = app.window; const auto treeB = app.tree();
    const auto viewerB = FindWindowExW(surfaceB, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return nextItem(treeB, TreeRelation::Child, nextItem(treeB, TreeRelation::Root)) != 0; }, "Project B did not load");
    send(treeB, treeSelect, nextItem(treeB, TreeRelation::Child, nextItem(treeB, TreeRelation::Root))); waitLines(viewerB, 300); keepTab(app, 0);
    open(c.root); check(app.window!=surfaceA && app.window!=surfaceB,"Projects reused a surface");
    select(0); check(app.window==surfaceA && nextItem(treeA,TreeRelation::Root)==rootA,"Switching rebuilt the explorer");
    check(lastTextRow(viewerA)==200 && scrollInfo(viewerA).nPos==top,"A-B-A lost the document or its view position");
    const auto terminals = surfaceA;check(terminals!=nullptr,"Project has no terminal pane");
    HWND shell = FindWindowExW(terminals,nullptr,L"PoppyTerminal",nullptr);check(shell!=nullptr,"Project has no shell");
    const auto before = childProcesses(app.process.dwProcessId);
    click(shell,10,10); // Start only this project's shell.
    DWORD shellPid = 0;
    waitFor([&] { for (const auto child : childProcesses(app.process.dwProcessId)) if (!before.count(child)) shellPid = child; return shellPid != 0; }, "Shell process did not start");
    const auto shellProcess = OpenProcess(SYNCHRONIZE, FALSE, shellPid); check(shellProcess != nullptr, "Cannot inspect the running shell");
    const auto px=[dpi=GetDpiForWindow(app.host)](int v){return MulDiv(v,dpi,96);};
    addTerminal(app, shell);
    check(FindWindowExW(terminals,shell,L"PoppyTerminal",nullptr)!=nullptr,"Adding a shell replaced the first session");
    select(1);
    writeFile(a.root / L"background.txt");
    waitFor([&] { return send(treeA, treeGetCount) == 3; }, "An inactive project's directory changes were not applied");
    check(lastTextRow(viewerB) == 300 && app.window == surfaceB, "An inactive result changed the visible project");
    select(0);check(IsWindow(shell) && WaitForSingleObject(shellProcess, 0) == WAIT_TIMEOUT,"Switching destroyed a running terminal");
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); double maximum = 0;
    for (int i = 0; i < 6; ++i) { LARGE_INTEGER start{}, end{}; QueryPerformanceCounter(&start); select(i % 2); QueryPerformanceCounter(&end); maximum = std::max(maximum, (end.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart); }
    std::cout << "Retained project switch maximum: " << maximum << " ms\n";
    send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(60),px(68)));
    send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(60),px(44+3*projectRowHeight-1)));
    send(app.host,WM_LBUTTONUP,0,MAKELPARAM(px(60),px(44+3*projectRowHeight-1)));
    check(app.tree()==treeA && app.window==surfaceA && scrollInfo(viewerA).nPos==top &&
          IsWindow(shell) && WaitForSingleObject(shellProcess,0)==WAIT_TIMEOUT,"Reordering a project lost its document or running terminal");
    select(0);check(app.window==surfaceB,"Dragging a project did not change the row order");
    send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(60),px(44+2*projectRowHeight+24)));
    send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(60),px(45)));
    send(app.host,WM_LBUTTONUP,0,MAKELPARAM(px(60),px(45)));
    select(0);check(app.window==surfaceA,"Dragging a project back to the first row failed");
    open(a.root);check(app.window==surfaceA,"Reopening a registered path duplicated the project");
    const auto oldB=surfaceB;const auto dpi=GetDpiForWindow(app.host);click(app.host,MulDiv(60,dpi,96),MulDiv(44+projectRowHeight+24,dpi,96),true);
    check(!IsWindow(oldB)&&IsWindow(surfaceA)&&IsWindow(shell) && WaitForSingleObject(shellProcess, 0) == WAIT_TIMEOUT,"Closing an inactive project affected another session");
    CloseHandle(shellProcess);
    }
    {
        Application restored(executable); const auto tree = restored.tree();
        waitLines(FindWindowExW(restored.window, nullptr, L"PoppyViewer", nullptr), 200);
        size_t count = 0; for (HWND project = nullptr; (project = FindWindowExW(restored.host, project, L"PoppyProject", nullptr));) ++count;
        check(count == 2 && nextItem(tree, TreeRelation::Root), "Restart lost the open list or reopened a closed project");
        const auto pane = restored.window;
        count = 0; for (HWND terminal = nullptr; (terminal = FindWindowExW(pane, terminal, L"PoppyTerminal", nullptr));) ++count;
        check(count == 2, "Restart lost terminal configuration");
        check(childProcesses(restored.process.dwProcessId).empty(), "Restoring terminal configuration automatically started a process");
    }
}
void recentProjectTests(const fs::path& executable) {
    Fixture fixture;
    const auto a = fixture.root / L"左 日本語" / L"project", b = fixture.root / L"右 日本語" / L"project";
    fs::create_directories(a); fs::create_directories(b);
    writeLines(a / L"a.txt", 200); writeLines(b / L"b.txt", 300);
    const auto findHost = [&](Application& app) {
        waitFor([&] {
            EnumWindows([](HWND candidate, LPARAM context) -> BOOL {
                auto& app = *reinterpret_cast<Application*>(context);
                DWORD id = 0; GetWindowThreadProcessId(candidate, &id);
                wchar_t name[32]{}; GetClassNameW(candidate, name, 32);
                if (id == app.process.dwProcessId && std::wstring(name) == L"PoppyMain") app.host = candidate;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&app));
            return app.host && IsWindowVisible(app.host);
        }, "Empty workspace did not appear");
    };
    const auto count = [](HWND host) {
        size_t result = 0;
        for (HWND project = nullptr; (project = FindWindowExW(host, project, L"PoppyProject", nullptr));) ++result;
        return result;
    };
    const auto show = [&](Application& app, HWND target) {
        const auto palette = FindWindowExW(app.host, nullptr, L"PoppyQuickOpen", L"Recent projects");
        check(palette != nullptr, "Recent projects palette is missing");
        // Share the application's input state while posting a real message-loop
        // shortcut. This also verifies Ctrl+R from editor/tree focus.
        struct Input {
            DWORD thread; BYTE previous[256]{};
            explicit Input(DWORD target) : thread(target) {
                check(AttachThreadInput(GetCurrentThreadId(), thread, TRUE) != FALSE, "Cannot attach shortcut input");
                GetKeyboardState(previous);
            }
            ~Input() { SetKeyboardState(previous); AttachThreadInput(GetCurrentThreadId(), thread, FALSE); }
        } input(app.process.dwThreadId);
        SetFocus(target);
        BYTE keys[256]{}; keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        check(SetKeyboardState(keys) != FALSE && PostMessageW(target, WM_KEYDOWN, 'R', 0) != FALSE, "Cannot post Ctrl+R");
        waitFor([&] { return IsWindowVisible(palette) != FALSE; }, "Ctrl+R did not open recent projects");
        return palette;
    };
    const auto type = [](HWND palette, const std::wstring& query) { for (const auto ch : query) send(palette, WM_CHAR, ch); };
    {
        Application app(executable); findHost(app);
        auto palette = show(app, app.host);
        send(palette, WM_KEYDOWN, VK_RETURN);
        check(IsWindowVisible(palette) && count(app.host) == 0, "Empty history opened a project");
        send(palette, WM_KEYDOWN, VK_ESCAPE);
        const auto open = [&](const fs::path& path, int lines) {
            const auto text = path.native();
            COPYDATASTRUCT data{0x50505901, static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)), const_cast<wchar_t*>(text.c_str())};
            check(send(app.host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)), "Cannot add a history test project");
            const auto tree = app.tree(); const auto root = nextItem(tree, TreeRelation::Root);
            waitFor([&] { return nextItem(tree, TreeRelation::Child, root) != 0; }, "History test project did not load");
            send(tree, treeSelect, nextItem(tree, TreeRelation::Child, root));
            waitLines(FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr), lines);
            keepTab(app, 0);
            return app.window;
        };
        const auto surfaceA = open(a, 200), surfaceB = open(b, 300);
        const auto treeB = app.tree();
        const auto px = [dpi = GetDpiForWindow(app.host)](int value) { return MulDiv(value, dpi, 96); };
        palette = show(app, treeB); type(palette, L"zz-no-such-history-97216");
        send(palette, WM_KEYDOWN, VK_RETURN);
        check(IsWindowVisible(palette) && count(app.host) == 2 && IsWindowVisible(surfaceB), "Unmatched history opened a stale project");
        send(palette, WM_KEYDOWN, VK_ESCAPE);
        check(!IsWindowVisible(palette) && IsWindowVisible(surfaceB), "Cancelling history switched projects");
        GUITHREADINFO focus{}; focus.cbSize = sizeof(focus);
        check(GetGUIThreadInfo(app.process.dwThreadId, &focus) && focus.hwndFocus == treeB, "Cancelling history did not restore focus");
        palette = show(app, treeB);
        check(PostMessageW(surfaceB, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px(12), px(22))) != FALSE, "Cannot click outside history");
        waitFor([&] { return !IsWindowVisible(palette); }, "An outside click did not close recent projects");
        // The second row opens an existing project instead of creating a copy.
        palette = show(app, FindWindowExW(surfaceB, nullptr, L"PoppyViewer", nullptr)); send(palette, WM_KEYDOWN, VK_DOWN); send(palette, WM_KEYDOWN, VK_RETURN);
        app.tree(); check(app.window == surfaceA && count(app.host) == 2, "History duplicated an already open project");
        click(app.host, px(60), px(44+projectRowHeight+24), true);
        check(!IsWindow(surfaceB) && count(app.host) == 1, "History test could not close project B");
        palette = show(app, FindWindowExW(surfaceA, nullptr, L"PoppyViewer", nullptr));
        type(palette, L"右 日本語"); click(palette, px(24), px(108));
        app.tree(); waitLines(FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr), 300);
        check(count(app.host) == 2 && !IsWindowVisible(palette), "History could not reopen a closed project by path");
        // Close both, leaving a saved workspace with no open project.
        click(app.host, px(60), px(44+projectRowHeight+24), true); click(app.host, px(60), px(68), true);
        check(count(app.host) == 0, "History test did not close all projects");
    }
    {
        Application app(executable); findHost(app);
        check(count(app.host) == 0, "Restart automatically opened a history entry");
        const auto palette = show(app, app.host); type(palette, L"左 日本語"); send(palette, WM_KEYDOWN, VK_RETURN);
        app.tree(); waitLines(FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr), 200);
        check(count(app.host) == 1 && !IsWindowVisible(palette), "Closed project history did not survive restart");
    }
}
void projectDragTests(const fs::path& executable) {
    Fixture a,b,c;
    writeLines(a.root/L"a.txt",200);writeLines(b.root/L"b.txt",300);writeLines(c.root/L"c.txt",400);
    {
        Application app(executable,a.root);
        const auto loadFile=[&](int lines){
            const auto tree=app.tree();const auto root=nextItem(tree,TreeRelation::Root);
            waitFor([&]{return nextItem(tree,TreeRelation::Child,root)!=0;},"A drag test project did not load");
            send(tree,treeSelect,nextItem(tree,TreeRelation::Child,root));
            waitLines(FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr),lines);
            keepTab(app, 0);
            return app.window;
        };
        const auto open=[&](const fs::path& path,int lines){
            const auto text=path.native();COPYDATASTRUCT data{0x50505901,static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)),const_cast<wchar_t*>(text.c_str())};
            check(send(app.host,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data)),"Could not add a drag test project");
            return loadFile(lines);
        };
        const auto surfaceA=loadFile(200),surfaceB=open(b.root,300),surfaceC=open(c.root,400);
        const auto px=[dpi=GetDpiForWindow(app.host)](int value){return MulDiv(value,dpi,96);};
        const auto point=[&](int row,int offset){return POINT{px(60),px(44+row*projectRowHeight+offset)};};
        const auto order=[&](std::initializer_list<HWND> expected){
            int index=0;for(const auto surface:expected){const auto p=point(index++,24);click(app.host,p.x,p.y);app.tree();check(app.window==surface,"Project drag produced the wrong order");}
        };
        const auto begin=[&](int from,POINT target){
            const auto p=point(from,24);send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(p.x,p.y));
            send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(target.x,target.y));
        };
        const auto drop=[&](POINT target){send(app.host,WM_LBUTTONUP,0,MAKELPARAM(target.x,target.y));};
        begin(0,point(2,projectRowHeight-1));drop(point(2,projectRowHeight-1));app.tree();
        check(app.window==surfaceA,"Reordering changed the selected project");order({surfaceB,surfaceC,surfaceA});
        begin(2,point(0,1));drop(point(0,1));order({surfaceA,surfaceB,surfaceC});
        begin(0,point(0,projectRowHeight-1));drop(point(0,projectRowHeight-1));order({surfaceA,surfaceB,surfaceC});
        auto tiny=point(0,24);++tiny.x;begin(0,tiny);drop(tiny);order({surfaceA,surfaceB,surfaceC});
        for(const auto cancel:{WM_CANCELMODE,WM_CAPTURECHANGED,WM_KEYDOWN}){
            begin(0,point(2,projectRowHeight-1));send(app.host,cancel,cancel==WM_KEYDOWN?VK_ESCAPE:0);drop(point(2,projectRowHeight-1));order({surfaceA,surfaceB,surfaceC});
        }
        RECT client{};GetClientRect(app.host,&client);
        for(const POINT outside: {POINT{px(300),px(80)},POINT{px(60),px(1)},POINT{px(60),client.bottom}}){
            begin(0,point(2,projectRowHeight-1));drop(outside);order({surfaceA,surfaceB,surfaceC});
        }
        // Escape must work while a project's editor owns keyboard focus.
        begin(0,point(2,projectRowHeight-1));
        check(PostMessageW(FindWindowExW(surfaceA,nullptr,L"PoppyViewer",nullptr),WM_KEYDOWN,VK_ESCAPE,0)!=FALSE,"Could not post drag cancellation");
        waitFor([&]{GUITHREADINFO info{};info.cbSize=sizeof(info);check(GetGUIThreadInfo(app.process.dwThreadId,&info)!=FALSE,"Could not inspect project capture");return info.hwndCapture==nullptr;},"Escape did not release project drag capture");
        drop(point(2,projectRowHeight-1));order({surfaceA,surfaceB,surfaceC});
        // Blank space after the last project accepts an append operation.
        begin(1,point(4,24));drop(point(4,24));order({surfaceA,surfaceC,surfaceB});
    }
    {
        Application app(executable);app.tree();
        waitLines(FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr),300);
        const auto px=[dpi=GetDpiForWindow(app.host)](int value){return MulDiv(value,dpi,96);};
        int index=0;for(const int lines:{200,400,300}){
            click(app.host,px(60),px(44+index++*projectRowHeight+24));app.tree();
            waitLines(FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr),lines);
        }
    }
}
void scrollingProjectDragTests(const fs::path& executable) {
    Fixture fixture;HWND surfaces[12]{};
    for(int i=0;i<12;++i){const auto path=fixture.root/std::to_wstring(i);fs::create_directory(path);writeFile(path/L"test.txt");}
    Application app(executable,fixture.root/L"0");app.tree();surfaces[0]=app.window;
    const auto scrollbar=FindWindowExW(app.host,nullptr,L"PoppyProjectScrollbar",nullptr);
    check(scrollbar&&!IsWindowVisible(scrollbar),"A fitting project list showed a scrollbar");
    for(int i=1;i<12;++i){
        const auto text=(fixture.root/std::to_wstring(i)).native();
        COPYDATASTRUCT data{0x50505901,static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)),const_cast<wchar_t*>(text.c_str())};
        check(send(app.host,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data)),"Could not add a scrolling project");app.tree();surfaces[i]=app.window;
    }
    const auto px=[dpi=GetDpiForWindow(app.host)](int value){return MulDiv(value,dpi,96);};
    check(SetWindowPos(app.host,nullptr,0,0,px(1000),px(480),SWP_NOMOVE|SWP_NOZORDER)!=FALSE,"Could not resize the project drag test");
    RECT client{};GetClientRect(app.host,&client);POINT wheel{px(60),px(68)};ClientToScreen(app.host,&wheel);
    const int page=(client.bottom-px(44))/px(projectRowHeight);
    const auto barBounds=bounds(app.host,scrollbar);
    check(IsWindowVisible(scrollbar)&&barBounds.top==px(44)&&barBounds.bottom==client.bottom&&
        barBounds.right==px(180)&&barBounds.right-barBounds.left==GetSystemMetricsForDpi(SM_CXVSCROLL,GetDpiForWindow(app.host)),
        "Overflowing projects did not show a scrollbar beside the cards below the header");
    check(scrollInfo(scrollbar).nMax==11&&scrollInfo(scrollbar).nPage==static_cast<UINT>(page),"Project scrollbar range did not follow the viewport height");
    send(app.host,WM_MOUSEWHEEL,MAKEWPARAM(0,12000),MAKELPARAM(wheel.x,wheel.y));
    check(scrollInfo(scrollbar).nPos==0,"Project wheel scrolling did not update the scrollbar");
    const POINT bottom{px(60),client.bottom-px(1)};
    send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(60),px(68)));
    send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bottom.x,bottom.y));
    for(int i=0;i<20;++i)send(app.host,WM_TIMER,1);
    send(app.host,WM_LBUTTONUP,0,MAKELPARAM(bottom.x,bottom.y));app.tree();
    check(app.window==surfaces[0],"Scrolling a dragged project lost its selection");
    send(app.host,WM_MOUSEWHEEL,MAKEWPARAM(0,12000),MAKELPARAM(wheel.x,wheel.y));
    click(app.host,px(60),px(68));app.tree();check(app.window==surfaces[1],"Auto-scroll did not move the first project to the end");
    send(app.host,WM_MOUSEWHEEL,MAKEWPARAM(0,-12000),MAKELPARAM(wheel.x,wheel.y));
    const int lastY=px(44)+(page-1)*px(projectRowHeight)+px(24);
    send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(60),lastY));
    send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(60),px(45)));
    for(int i=0;i<20;++i)send(app.host,WM_TIMER,1);
    send(app.host,WM_LBUTTONUP,0,MAKELPARAM(px(60),px(45)));
    click(app.host,px(60),px(68));app.tree();check(app.window==surfaces[0],"Auto-scroll did not move the last project to the start");

    RECT bar{};GetClientRect(scrollbar,&bar);const int x=bar.right/2,arrow=bar.right;
    click(scrollbar,x,bar.bottom-1);
    check(scrollInfo(scrollbar).nPos==1,"Project scrollbar down arrow did not scroll one card");
    click(scrollbar,x,1);
    check(scrollInfo(scrollbar).nPos==0,"Project scrollbar up arrow did not return to the first card");
    click(scrollbar,x,bar.bottom-arrow-1);
    check(scrollInfo(scrollbar).nPos==page,"Project scrollbar track did not scroll one page");
    click(scrollbar,x,arrow+1);
    check(scrollInfo(scrollbar).nPos==0,"Project scrollbar track did not scroll back one page");
    send(scrollbar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,arrow+1));
    send(scrollbar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,bar.bottom-arrow-1));
    send(scrollbar,WM_LBUTTONUP,0,MAKELPARAM(x,bar.bottom-arrow-1));
    check(scrollInfo(scrollbar).nPos==12-page,"Dragging the project scrollbar did not reach the last card");
    send(scrollbar,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(wheel.x,wheel.y));
    check(scrollInfo(scrollbar).nPos==std::max(0,12-page-3),"Wheel input over the project scrollbar did not scroll the cards");
    app.tree();check(app.window==surfaces[0],"Using the project scrollbar changed the active project");
    const int firstVisible=scrollInfo(scrollbar).nPos;
    click(app.host,px(60),px(68));app.tree();
    check(app.window==surfaces[firstVisible],"The project cards did not follow the scrollbar position");
    for(const int index:{0,11}){
        const auto text=(fixture.root/std::to_wstring(index)).native();
        COPYDATASTRUCT data{0x50505901,static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)),const_cast<wchar_t*>(text.c_str())};
        check(send(app.host,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data)),"Could not select a project outside the viewport");app.tree();
        check(app.window==surfaces[index]&&scrollInfo(scrollbar).nPos==(index?12-page:0),"Selecting a hidden project did not synchronize the scrollbar");
    }
    for(int remaining=12;remaining>page;--remaining){
        send(scrollbar,WM_VSCROLL,SB_TOP);
        click(app.host,px(60),px(68),true);app.tree();
        check((IsWindowVisible(scrollbar)!=FALSE)==(remaining-1>page),"Closing projects did not update scrollbar visibility");
    }
    check(scrollInfo(scrollbar).nPos==0,"A fitting project list retained its old scroll position");
}
void terminalTitleTests(const fs::path& executable) {
    Fixture fixture; writeFile(fixture.root / L"test.txt");
    {
        Application app(executable, fixture.root); app.tree();
        const auto pane = app.window;
        const auto shell = FindWindowExW(pane, nullptr, L"PoppyTerminal", nullptr);
        check(shell != nullptr, "Project has no shell for title tests");
        const auto name = [](HWND window) { wchar_t text[512]{}; GetWindowTextW(window, text, 512); return std::wstring(text); };
        const auto type = [](HWND window, const std::wstring& text) { for (const auto ch : text) send(window, WM_CHAR, ch); };
        check(name(shell) == L"PowerShell", "Unstarted terminal did not show its shell name");
        click(shell, 10, 10);
        type(shell, L"[Console]::Title = 'Build 日本語'\r");
        waitFor([&] { return name(shell) == L"Build 日本語"; }, "ConPTY title did not reach the terminal tab");
        addTerminal(app, shell);
        const auto second = FindWindowExW(pane, shell, L"PoppyTerminal", nullptr);
        check(second && name(second) == L"PowerShell", "New terminal inherited another session's title");
        click(second, 10, 10);
        type(second, L"[Console]::Title = 'Second title'\r");
        waitFor([&] { return name(second) == L"Second title"; }, "Second terminal did not receive its own title");
        check(!IsWindowVisible(shell), "Adding a terminal did not hide the first session");
        type(shell, L"[Console]::Title = 'Background title'\r");
        waitFor([&] { return name(shell) == L"Background title"; }, "Hidden terminal stopped updating its title");
        check(name(second) == L"Second title", "Background title changed the active terminal's name");
        clickTerminalTab(app, shell);

        check(PostMessageW(app.window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(app.window), MAKELPARAM(-1, -1)), "Cannot open terminal context menu");
        waitFor([&] { GUITHREADINFO info{}; info.cbSize = sizeof(info); return GetGUIThreadInfo(app.process.dwThreadId, &info) && (info.flags & GUI_INMENUMODE); }, "Terminal context menu did not open");
        PostMessageW(pane, WM_KEYDOWN, VK_HOME, 0); PostMessageW(pane, WM_KEYDOWN, VK_RETURN, 0);
        HWND edit = nullptr;
        waitFor([&] { edit = GetDlgItem(pane, 1001); return edit != nullptr; }, "Terminal rename did not open");
        wchar_t initial[512]{}; send(edit, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(initial));
        check(std::wstring(initial) == L"Background title", "Rename did not start with the displayed process title");
        send(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"Pinned name")); send(edit, WM_KEYDOWN, VK_RETURN);
        waitFor([&] { return name(shell) == L"Pinned name"; }, "Manual terminal name was not applied");
        type(shell, L"[Console]::Title = 'Automatic replacement'\r");
        const auto started = GetTickCount64();
        waitFor([&] { check(name(shell) == L"Pinned name", "Process title overwrote a manual terminal name"); return GetTickCount64() - started >= 300; }, "Manual terminal title did not remain stable");
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto pane = app.window;
        const auto first = FindWindowExW(pane, nullptr, L"PoppyTerminal", nullptr);
        const auto second = FindWindowExW(pane, first, L"PoppyTerminal", nullptr);
        wchar_t text[128]{}; GetWindowTextW(first, text, 128);
        check(std::wstring(text) == L"Pinned name", "Restart lost the manual terminal name");
        GetWindowTextW(second, text, 128);
        check(second && std::wstring(text) == L"PowerShell", "Restart persisted a transient process title");
    }
}
void terminalShellTests(const fs::path& executable) {
    Fixture fixture; writeFile(fixture.root / L"test.txt");
    const auto name = [](HWND window) { wchar_t text[512]{}; GetWindowTextW(window, text, 512); return std::wstring(text); };
    const auto sessions = [](HWND pane) {
        std::vector<HWND> result;
        for (HWND child = nullptr; (child = FindWindowExW(pane, child, L"PoppyTerminal", nullptr));) result.push_back(child);
        return result;
    };
    const auto choose = [](const Application& app, HWND pane, std::initializer_list<WPARAM> keys) {
        HWND active = nullptr;
        for (HWND child = nullptr; (child = FindWindowExW(pane, child, L"PoppyTerminal", nullptr));) if (IsWindowVisible(child)) active = child;
        const auto point = terminalAddPoint(app, active, true);
        check(PostMessageW(pane, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y)), "Could not click the shell dropdown");
        const auto open = [&] { GUITHREADINFO info{}; info.cbSize = sizeof(info); return GetGUIThreadInfo(app.process.dwThreadId, &info) && (info.flags & GUI_INMENUMODE); };
        waitFor(open, "Shell dropdown did not open");
        for (auto key : keys) PostMessageW(pane, WM_KEYDOWN, key, 0);
        waitFor([&] { return !open(); }, "Shell dropdown did not close");
        send(pane, WM_NULL);
    };
    const auto plus = [](const Application& app, HWND pane) {
        for (HWND child = nullptr; (child = FindWindowExW(pane, child, L"PoppyTerminal", nullptr));) if (IsWindowVisible(child)) { addTerminal(app, child); return; }
    };
    {
        Application app(executable, fixture.root); app.tree();
        const auto pane = app.window;
        choose(app, pane, {VK_ESCAPE});
        check(sessions(pane).size() == 1, "Cancelling shell selection added a terminal");
        choose(app, pane, {VK_HOME, VK_DOWN, VK_RETURN});
        auto tabs = sessions(pane);
        check(tabs.size() == 2 && name(tabs[1]) == L"Git Bash", "Git Bash selection did not add its own terminal");
        check(name(tabs[0]) == L"PowerShell", "Shell selection changed an existing terminal");
        if (fs::is_regular_file(L"C:\\Program Files\\Git\\bin\\bash.exe")) {
            click(tabs[1], 10, 10);
            for (const auto ch : std::wstring(L"printf ready > bash-ready.txt\r")) send(tabs[1], WM_CHAR, ch);
            waitFor([&] { std::ifstream file(fixture.root/L"bash-ready.txt"); std::string text; return file && std::getline(file,text) && text=="ready"; }, "Git Bash did not run in the Unicode project directory");
            for (const auto ch : std::wstring(L"printf '%s' 'hello world\x7f' > bash-word.txt\r")) send(tabs[1], WM_CHAR, ch);
            waitFor([&] { std::ifstream file(fixture.root/L"bash-word.txt"); std::string text; return file && std::getline(file,text,'\0') && text=="hello "; }, "Ctrl+Backspace did not delete a word in Git Bash");
            for (const auto ch : std::wstring(L"printf '%s' 'hello world\b' > bash-character.txt\r")) send(tabs[1], WM_CHAR, ch);
            waitFor([&] { std::ifstream file(fixture.root/L"bash-character.txt"); std::string text; return file && std::getline(file,text,'\0') && text=="hello worl"; }, "Backspace did not delete one character in Git Bash");
        }
        choose(app, pane, {VK_END, VK_RIGHT, VK_END, VK_RETURN});
        check(sessions(pane).size() == 2, "Changing the default added a terminal");
        plus(app, pane); tabs = sessions(pane);
        check(tabs.size() == 3 && name(tabs[2]) == L"Git Bash", "Plus did not use the Git Bash default");
        choose(app, pane, {VK_HOME, VK_RETURN}); tabs = sessions(pane);
        check(tabs.size() == 4 && name(tabs[3]) == L"PowerShell", "Explicit PowerShell selection ignored the chosen profile");
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto pane = app.window;
        auto tabs = sessions(pane);
        check(tabs.size() == 4 && name(tabs[0]) == L"PowerShell" && name(tabs[1]) == L"Git Bash" &&
              name(tabs[2]) == L"Git Bash" && name(tabs[3]) == L"PowerShell", "Restart lost terminal shell profiles");
        check(childProcesses(app.process.dwProcessId).empty(), "Restoring shell profiles automatically started a process");
        plus(app, pane); tabs = sessions(pane);
        check(tabs.size() == 5 && name(tabs[4]) == L"Git Bash", "Restart lost the default or explicit selection changed it");
        choose(app, pane, {VK_END, VK_RIGHT, VK_HOME, VK_RETURN});
        plus(app, pane); tabs = sessions(pane);
        check(tabs.size() == 6 && name(tabs[5]) == L"PowerShell", "Could not switch the default back to PowerShell");
    }
}
void terminalShortcutTests(const fs::path& executable) {
    Fixture fixture; writeLines(fixture.root / L"test.txt", 200);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    const auto root = nextItem(tree, TreeRelation::Root);
    waitFor([&] { return nextItem(tree, TreeRelation::Child, root) != 0; }, "Shortcut file did not load");
    send(tree, treeSelect, nextItem(tree, TreeRelation::Child, root)); waitLines(viewer, 200);
    const auto pane = app.window;
    const auto first = FindWindowExW(pane, nullptr, L"PoppyTerminal", nullptr);
    check(first != nullptr, "Shortcut terminal is missing");
    addTerminal(app, first);
    const auto second = FindWindowExW(pane, first, L"PoppyTerminal", nullptr);
    check(second != nullptr, "Second shortcut terminal is missing");
    const auto body = bounds(app.window, second);
    dragTerminalTab(app, first, {body.left + 10, (body.top + body.bottom) / 2});
    check(IsWindowVisible(first) && IsWindowVisible(second), "Shortcut terminals did not split");
    struct Input {
        DWORD thread; BYTE previous[256]{};
        explicit Input(DWORD target) : thread(target) {
            check(AttachThreadInput(GetCurrentThreadId(), thread, TRUE) != FALSE, "Cannot attach terminal shortcut input");
            GetKeyboardState(previous);
        }
        ~Input() { SetKeyboardState(previous); AttachThreadInput(GetCurrentThreadId(), thread, FALSE); }
    } input(app.process.dwThreadId);
    struct Shortcut { WPARAM key; bool ctrl, shift; };
    const Shortcut shortcuts[] = {{'O', true, false}, {'J', true, false}, {'R', true, false}, {'N', true, false},
        {'P', true, false}, {'F', true, false}, {'W', true, false},
        {'F', true, true}, {'E', true, true}, {'G', true, true}, {'B', true, true},
        {VK_F3, false, false}, {VK_F5, false, false}, {VK_F6, false, false}, {VK_F7, false, false}};
    for (const auto shell : {first, second}) {
        click(shell, 10, 10);
        // Read actual ConPTY input, rather than sending WM_CHAR directly: the
        // application's message loop must translate and dispatch each key.
        const auto command = L"for ($i=0; $i -lt " + std::to_wstring(sizeof(shortcuts) / sizeof(shortcuts[0])) +
            L"; $i++) { [Console]::Title = ('shortcut-' + $i); $k = [Console]::ReadKey($true); "
            L"if ($i -eq 0 -and [int]$k.KeyChar -ne 15) { throw 'Ctrl+O was changed' }; "
            L"if ($i -eq 1 -and [int]$k.KeyChar -ne 10) { throw 'Ctrl+J was changed' } }; [Console]::Title = 'shortcut-done'\r";
        for (const auto ch : command) send(shell, WM_CHAR, ch);
        const auto waitTitle = [&](const std::wstring& expected) {
            waitFor([&] { wchar_t title[128]{}; GetWindowTextW(shell, title, 128); return title == expected; },
                "An application shortcut intercepted terminal input");
        };
        size_t index = 0;
        for (const auto& shortcut : shortcuts) {
            waitTitle(L"shortcut-" + std::to_wstring(index++));
            BYTE keys[256]{};
            if (shortcut.ctrl) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
            if (shortcut.shift) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
            check(SetKeyboardState(keys) && PostMessageW(shell, WM_KEYDOWN, shortcut.key, 0), "Cannot post a terminal shortcut");
        }
        waitTitle(L"shortcut-done");
        BYTE keys[256]{}; SetKeyboardState(keys);
        GUITHREADINFO focus{}; focus.cbSize = sizeof(focus);
        check(GetGUIThreadInfo(app.process.dwThreadId, &focus) && focus.hwndFocus == shell, "A shortcut moved terminal focus");
        check(IsWindowVisible(first) && IsWindowVisible(second) && IsWindowVisible(tree) && visibleTabCount(app) == 1, "A terminal shortcut changed application panes or tabs");
    }
}
void terminalBackspaceTests(const fs::path& executable) {
    Fixture fixture;
    writeFile(fixture.root / L"test.txt");
    Application app(executable, fixture.root);
    app.tree();
    const auto pane = app.window;
    const auto shell = FindWindowExW(pane, nullptr, L"PoppyTerminal", nullptr);
    check(shell != nullptr, "Project has no shell for Backspace tests");
    click(shell, 10, 10);
    const auto type = [&](const std::wstring& text) { for (const auto ch : text) send(shell, WM_CHAR, ch); };
    const auto waitText = [&](const fs::path& path, const std::string& expected, const char* message) {
        waitFor([&] {
            std::ifstream file(path, std::ios::binary);
            std::string text;
            return file && std::getline(file, text, '\0') && text == expected;
        }, message);
    };
    type(L"Set-Content -NoNewline -Encoding Ascii ready.txt ready\r");
    waitText(fixture.root / L"ready.txt", "ready", "PowerShell did not become ready for Backspace tests");

    // WM_CHAR carries BS for Backspace and DEL for Ctrl+Backspace.
    // Check the resulting PowerShell input after it passes through ConPTY.
    type(L"'hello world\b' | Set-Content -NoNewline -Encoding Ascii backspace.txt\r");
    waitText(fixture.root / L"backspace.txt", "hello worl", "Backspace did not delete exactly one character");
    type(L"'hello world\b\b\b\b\b\b\b' | Set-Content -NoNewline -Encoding Ascii repeated.txt\r");
    waitText(fixture.root / L"repeated.txt", "hell", "Repeated Backspace did not delete one character at a time across a word boundary");
    type(L"'hello world\x7f' | Set-Content -NoNewline -Encoding Ascii word.txt\r");
    waitText(fixture.root / L"word.txt", "hello ", "Ctrl+Backspace did not delete the preceding word");
    type(L"'hello brave world\x7f\x7f' | Set-Content -NoNewline -Encoding Ascii words.txt\r");
    waitText(fixture.root / L"words.txt", "hello ", "Repeated Ctrl+Backspace did not delete words one at a time");
}
void sessionTests(const fs::path& executable) {
    Fixture fixture;
    const auto directory = fixture.root / L"nested 日本語";
    fs::create_directory(directory);
    writeLines(directory / L"a.txt", 200); writeLines(directory / L"b.txt", 300); writeLines(directory / L"c.txt", 100);
    int firstTop = 0, firstLeft = 0, secondTop = 0, sidebarEdge = 0;
    {
        Application app(executable, fixture.root);
        const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        TreeItem nested = 0;
        waitFor([&] { nested = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root)); return nested != 0; }, "Session fixture did not load");
        expand(tree, nested);
        TreeItem first = 0, second = 0, third = 0;
        waitFor([&] {
            first = nextItem(tree, TreeRelation::Child, nested); second = nextItem(tree, TreeRelation::Next, first); third = nextItem(tree, TreeRelation::Next, second);
            return first && second && third;
        }, "Session fixture did not expand");
        send(tree, treeSelect, first); waitLines(viewer, 200); keepTab(app, 0);
        for (int i = 0; i < 12; ++i) { send(viewer, WM_VSCROLL, SB_LINEDOWN); send(viewer, WM_HSCROLL, SB_LINEDOWN); }
        firstTop = scrollInfo(viewer).nPos; firstLeft = scrollInfo(viewer, SB_HORZ).nPos;
        send(tree, treeSelect, second); waitLines(viewer, 300); keepTab(app, 1);
        send(viewer, WM_VSCROLL, SB_PAGEDOWN); secondTop = scrollInfo(viewer).nPos;
        send(tree, treeSelect, third); waitLines(viewer, 100); keepTab(app, 2);
        send(tree, treeSelect, second); waitLines(viewer, 300);
        const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
        const auto terminal = FindWindowExW(app.window, nullptr, L"PoppyTerminal", nullptr);
        check(terminal && IsWindowVisible(terminal), "Session terminal is missing");
        controlShortcut(app, viewer, 'J', [&] { return !IsWindowVisible(terminal); }, "Ctrl+J did not hide the session terminal");
        const auto pane = bounds(app.window, viewer);
        const int splitter = sidebarOnRight ? pane.right + px(2) : pane.left - px(3);
        const int y = pane.top + px(50);
        send(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(splitter, y));
        send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(splitter + px(60), y));
        send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(splitter + px(60), y));
        const auto resizedPane = bounds(app.window, viewer);
        sidebarEdge = sidebarOnRight ? resizedPane.right : resizedPane.left;
        const int originalEdge = sidebarOnRight ? pane.right : pane.left;
        check(sidebarEdge >= originalEdge + px(60) - 1 && sidebarEdge <= originalEdge + px(60) + 1,
              "Session test did not resize the sidebar");
        send(GetDlgItem(app.window, 3), BM_CLICK);
        check(!IsWindowVisible(tree), "Session test did not select the Git sidebar");
    }
    fs::remove(directory / L"c.txt");
    {
        // Omit the command-line path to exercise last-project restoration.
        Application app(executable);
        const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitLines(viewer, 300);
        check(scrollInfo(viewer).nPos == secondTop, "Restart lost the active tab or its scroll position");
        check(!IsWindowVisible(FindWindowExW(app.window, nullptr, L"PoppyTerminal", nullptr)), "Restart lost terminal visibility");
        check(!IsWindowVisible(tree), "Restart lost the active sidebar");
        const auto pane = bounds(app.window, viewer);
        check((sidebarOnRight ? pane.right : pane.left) == sidebarEdge, "Restart lost sidebar width");
        TreeItem nested = 0, first = 0;
        waitFor([&] {
            nested = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
            first = nextItem(tree, TreeRelation::Child, nested); return nested && expanded(tree, nested) && first;
        }, "Restart did not restore nested folder expansion");
        waitFor([&] { return static_cast<TreeItem>(send(tree, treeGetSelection)) == nextItem(tree, TreeRelation::Next, first); }, "Restart lost tree selection");
        const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
        click(app.window, pane.left + px(30), pane.top - px(17)); waitLines(viewer, 200);
        check(scrollInfo(viewer).nPos == firstTop && scrollInfo(viewer, SB_HORZ).nPos == firstLeft, "Restart lost an inactive tab's position");
        click(app.window, pane.left + px(30), pane.top - px(17), true); waitLines(viewer, 300);
        click(app.window, pane.left + px(30), pane.top - px(17), true);
        check(scrollInfo(viewer).nMax == 0, "Restart retained a missing file's tab");
        expand(tree, nextItem(tree, TreeRelation::Root), false);
    }
    {
        Application app(executable, fixture.root);
        const auto tree = app.tree();
        const auto root = nextItem(tree, TreeRelation::Root);
        check(root && !expanded(tree, root) && !nextItem(tree, TreeRelation::Child, root), "Restart reopened a collapsed root");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2 || (argc == 3 && std::wstring(argv[2]) == L"--dock-only"), "Pass the path to Poppy.exe");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (argc == 3) { Fixture appData; AppDataEnvironment environment(appData.root); dockLayoutTests(fs::absolute(argv[1])); return 0; }
        for (const auto test : {folderTests, explorerSortTests, sidebarTests, treeInteractionTests, collapseAllSidebarTests, navigationHistoryTests, previewTabTests, explorerPreviewTabTests, previewTabSplitTests, tabTests, rapidTabMiddleClickTests, pinnedTabTests,
                tabCopyPathTests, bulkTabCloseTests, bulkTabSaveTests, bulkTabSplitTests, explorerActiveFileTests, htmlTabToolsTests, editorTailTests,
                tabDragTests, wrappingTabDragTests, wrappingTabTests, renameTests, explorerMoveTests, explorerDeleteTests, explorerDeleteConfirmationTests, explorerMultipleSelectionTests, crossProjectFolderCopyTests, createItemTests, newFileShortcutTests, pasteImageTests, splitPaneTests, dockLayoutTests, quickOpenTests, fullSearchTests, searchRetentionTests, gitSidebarTests, gitIgnoredExplorerTests, gitAutoRefreshTests, editableGitDiffTests, branchSidebarTests, comparisonSidebarTests, multiRepositoryTests, sessionTests, multiProjectTests, recentProjectTests, projectDragTests, scrollingProjectDragTests, terminalTitleTests, terminalShellTests, terminalShortcutTests, terminalBackspaceTests}) {
            Fixture appData; AppDataEnvironment environment(appData.root);
            test(fs::absolute(argv[1]));
        }
        std::cout << "Folder, sidebar, Git layouts, tab, quick open, session and terminal input tests passed.\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
