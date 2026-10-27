#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <set>
#include "tree_messages.h"
#include "activity_button.h"
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace poppy;
namespace {
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
SCROLLINFO scrollInfo(HWND window, int bar = SB_VERT) {
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
    check(GetScrollInfo(window, bar, &info) != FALSE, "Could not read the viewer's scroll position");
    return info;
}
void waitLines(HWND viewer, int lines) {
    waitFor([&] { return scrollInfo(viewer).nMax == lines; }, "The selected tab did not load the expected document");
}
void click(HWND window, int x, int y, bool middle = false) {
    send(window, middle ? WM_MBUTTONDOWN : WM_LBUTTONDOWN, middle ? MK_MBUTTON : MK_LBUTTON, MAKELPARAM(x, y));
    send(window, middle ? WM_MBUTTONUP : WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}
void discardGroup(const Application& app, HWND changes, TreeItem group, int answer) {
    expand(changes, group, false);
    RECT rect{}; GetClientRect(changes, &rect);
    const int x = rect.right - MulDiv(74, GetDpiForWindow(app.window), 96);
    const int height = static_cast<int>(send(changes, treeGetRowHeight));
    const int y = (static_cast<int>(send(changes, treeGetRow, group)) - scrollInfo(changes).nPos) * height + height / 2;
    // Post the click because its handler opens a modal confirmation.
    PostMessageW(changes, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    PostMessageW(changes, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    PostMessageW(changes, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    HWND dialog = nullptr;
    waitFor([&] {
        dialog = FindWindowW(L"#32770", L"Poppy — Discard All Changes");
        DWORD pid = 0; if (dialog) GetWindowThreadProcessId(dialog, &pid);
        return dialog && pid == app.process.dwProcessId;
    }, "Discard All Changes did not ask for confirmation from a collapsed group");
    check(send(dialog, DM_GETDEFID) == MAKELONG(IDCANCEL, DC_HASDEFID), "Discard confirmation must default to Cancel");
    send(dialog, WM_COMMAND, answer);
    waitFor([&] { return !IsWindow(dialog); }, "Discard confirmation did not close");
}
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
int visibleTabCount(const Application& app) {
    return static_cast<int>(send(tabTooltips(app), TTM_GETTOOLCOUNT)) - 2;
}
RECT tabBounds(const Application& app, int visibleIndex) {
    // Tooltip regions expose the actual clickable bounds without assuming a
    // font's character widths. Tooltip messages need memory in Poppy's process.
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
    return tool.rect;
}
void clickTab(const Application& app, int visibleIndex, bool middle = false, bool close = false) {
    const auto rect = tabBounds(app, visibleIndex);
    const int inset = MulDiv(close ? 14 : 16, GetDpiForWindow(app.window), 96);
    click(app.window, close ? rect.right - inset : rect.left + inset, (rect.top + rect.bottom) / 2, middle);
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
void toggleTabPin(const Application& app, int visibleIndex) {
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
    // Select the first item (Pin tab / Unpin tab) through the native menu loop.
    PostMessageW(app.window, WM_KEYDOWN, VK_HOME, 0);
    PostMessageW(app.window, WM_KEYDOWN, VK_RETURN, 0);
    waitFor([&] { return !menuOpen(); }, "The tab context menu did not close");
    send(app.window, WM_NULL);
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
    send(tree, treeSelect, static_cast<WPARAM>(first)); waitLines(viewer, 200);
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
    check(scrollInfo(viewer).nMax == 220 && scrollInfo(viewer).nPos == firstTop, "Closing an inactive tab changed the active document");
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top && scrollInfo(viewer).nMax == 0, "Closing the last tab left a duplicate tab or document");
    // Reopen, then close a newly loading tab and its neighbor. Older results
    // must not repopulate the viewer or replace the next selected document.
    send(tree, treeSelect, static_cast<WPARAM>(first)); waitLines(viewer, 220);
    send(tree, treeSelect, static_cast<WPARAM>(second));
    clickTab(app, 1, true);
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top, "Closing tabs during loading did not clear the tab strip");
    // b.txt is still selected in the tree, so reopening also exercises Enter.
    send(tree, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top && scrollInfo(viewer).nMax == 0, "A stale load restored a closed tab");
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
            send(tree, treeSelect, item); waitLines(viewer, lines);
            item = nextItem(tree, TreeRelation::Next, item);
        }
        const auto singleRow = bounds(app.window, viewer);
        toggleTabPin(app, 1);
        check(scrollInfo(viewer).nMax == 400, "Pinning an inactive tab changed the active file");
        const auto pinned = tabBounds(app, 0), regular = tabBounds(app, 1);
        check(pinned.bottom == regular.top && pinned.left == regular.left,
              "Pinned and regular tabs did not start on separate rows");
        check(bounds(app.window, viewer).top == singleRow.top + pinned.bottom - pinned.top,
              "The viewer did not reserve a dedicated pinned tab row");
        clickTab(app, 0); waitLines(viewer, 300);
        clickTab(app, 0, true);
        check(visibleTabCount(app) == 3 && scrollInfo(viewer).nMax == 300, "Middle-click closed a pinned tab");
        clickTab(app, 0, false, true);
        check(visibleTabCount(app) == 3 && scrollInfo(viewer).nMax == 300, "A pinned tab retained a close button");
        toggleTabPin(app, 0);
        check(bounds(app.window, viewer).top == singleRow.top && tabBounds(app, 0).top == tabBounds(app, 1).top,
              "Unpinning the last pinned tab left an empty row");
        clickTab(app, 0, false, true); waitLines(viewer, 200);
        check(visibleTabCount(app) == 2, "An unpinned tab could not be closed");
        toggleTabPin(app, 1);
        check(scrollInfo(viewer).nMax == 200, "Moving a pinned tab lost the active file");
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
        check(scrollInfo(viewer).nMax == 200, "Unpinning an inactive tab changed the active file");
        check(tabBounds(app, 0).bottom == tabBounds(app, 1).top,
              "Unpinning a tab did not move it below the pinned row");
        clickTab(app, 1, true);
        check(visibleTabCount(app) == 1 && scrollInfo(viewer).nMax == 200, "Closing an unpinned neighbor changed the pinned file");
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
            send(tree, treeSelect, item); waitLines(viewer, lines);
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
        send(tree, treeSelect, item); waitLines(viewer, 100 + i);
        item = nextItem(tree, TreeRelation::Next, item);
    }
    const auto first = tabBounds(app, 0), last = tabBounds(app, 7);
    check(first.top < last.top, "The drag test tabs did not wrap");
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
    // Fire the repeat timer while hovering over the next-row button. The
    // source tab must stay captured even after its row is no longer visible.
    for (int i = 0; i < 8; ++i) send(app.window, WM_TIMER, 2);
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
        send(tree, treeSelect, static_cast<WPARAM>(item)); waitLines(viewer, 100 + i);
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
    check(scrollInfo(viewer).nMax == 107, "Clicking the empty part of a tab row changed the active file");
    // Close an inactive tab in the third row, then the active last-row tab.
    clickTab(app, 5, true);
    check(scrollInfo(viewer).nMax == 107, "Closing a wrapped inactive tab changed the document");
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
    check(scrollInfo(viewer).nMax == 106, "Scrolling the tab rows changed the active file");
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
    const auto start = [&](TreeItem parent, bool directory, const wchar_t* name) {
        send(tree, treeSelect, parent);
        check(PostMessageW(tree, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(tree), MAKELPARAM(-1, -1)), "Cannot open folder context menu");
        const auto menuOpen = [&] { GUITHREADINFO info{}; info.cbSize = sizeof(info); check(GetGUIThreadInfo(app.process.dwThreadId, &info), "Cannot inspect creation menu"); return (info.flags & GUI_INMENUMODE) != 0; };
        waitFor(menuOpen, "Folder context menu did not open");
        PostMessageW(tree, WM_KEYDOWN, VK_HOME, 0);
        if (directory) PostMessageW(tree, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(tree, WM_KEYDOWN, VK_RETURN, 0);
        waitFor([&] { return !menuOpen(); }, "Folder context menu did not close");
        HWND edit = nullptr;
        waitFor([&] { edit = FindWindowExW(tree, nullptr, L"EDIT", nullptr); return edit != nullptr; }, "Creation did not show the name input");
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
    check(visibleTabCount(app) == 1, "Created file did not open in the editor");
    edit = start(folder, true, L"空 フォルダー"); send(edit, WM_KEYDOWN, VK_RETURN);
    waitFor([&] { return fs::is_directory(created.parent_path() / L"空 フォルダー") && send(tree, treeGetCount) == 5; }, "New Folder did not create and display a folder");
    check(visibleTabCount(app) == 1, "Creating a folder opened a file tab");
    edit = start(root, true, L"cancelled"); send(edit, WM_KEYDOWN, VK_ESCAPE);
    check(!fs::exists(fixture.root / L"cancelled"), "Escape created a folder");
    start(root, false, L"focus-cancelled.txt"); click(tree, 30, 10);
    check(!FindWindowExW(tree, nullptr, L"EDIT", nullptr) && !fs::exists(fixture.root / L"focus-cancelled.txt"), "Focus loss created a file");
    expand(tree, root, false);
    edit = start(root, false, L"root.txt"); send(edit, WM_KEYDOWN, VK_RETURN);
    waitFor([&] { return fs::exists(fixture.root / L"root.txt") && expanded(tree, root); }, "Cannot create inside a collapsed project root");
    fail(false, L"root.txt"); fail(true, L"root.txt"); fail(false, L"collapsed 日本語"); fail(true, L"collapsed 日本語");
    fail(false, L"../escape.txt"); fail(true, L"CON"); fail(false, L"");
    check(fs::file_size(fixture.root / L"root.txt") == 0 && fs::is_directory(created.parent_path()) && fs::exists(created), "Creation collision changed an existing item");
    check(visibleTabCount(app) == 2, "Failed or cancelled creation changed file tabs");
}
void newFileShortcutTests(const fs::path& executable) {
    Fixture fixture; fs::create_directory(fixture.root / L"chosen 日本語");
    writeFile(fixture.root / L"chosen 日本語" / L"existing.txt");
    Application app(executable, fixture.root); const auto tree = app.tree();
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return send(tree, treeGetCount) == 2; }, "Shortcut fixture did not load");
    const auto root = nextItem(tree, TreeRelation::Root), folder = nextItem(tree, TreeRelation::Child, root);
    const auto start = [&](HWND target, const std::wstring& name) {
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
        waitFor([&] { edit = FindWindowExW(tree, nullptr, L"EDIT", nullptr); return edit != nullptr; }, "Ctrl+N did not show the path input");
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
    send(tree, treeSelect, root); expand(tree, root, false);
    const auto terminalPane = FindWindowExW(app.window, nullptr, L"PoppyTerminalPane", nullptr);
    const auto terminal = FindWindowExW(terminalPane, nullptr, L"PoppyTerminal", nullptr);
    check(terminal != nullptr, "Shortcut terminal is missing");
    edit = start(terminal, L"cancelled/path.txt"); send(edit, WM_KEYDOWN, VK_ESCAPE);
    check(!fs::exists(fixture.root / L"cancelled") && !FindWindowExW(tree, nullptr, L"EDIT", nullptr), "Escape created intermediate folders");
    edit = start(viewer, L"focus-cancelled/path.txt"); click(tree, 30, 10);
    check(!fs::exists(fixture.root / L"focus-cancelled") && !FindWindowExW(tree, nullptr, L"EDIT", nullptr), "Focus loss created intermediate folders");
    // The full input, not only one 255-character component, reaches creation.
    const auto longPath = std::wstring(130, L'a') + L"/" + std::wstring(130, L'b') + L"/long.txt";
    edit = start(terminal, longPath);
    check(GetWindowTextLengthW(edit) == static_cast<int>(longPath.size()), "Ctrl+N truncated a path longer than 255 characters");
    send(edit, WM_KEYDOWN, VK_ESCAPE);
    // Keep the filesystem check below MAX_PATH so the test does not depend on
    // the host's long-path registry setting.
    edit = start(terminal, L"root.txt"); commit(edit);
    waitFor([&] { return fs::is_regular_file(fixture.root / L"root.txt") && expanded(tree, root); }, "Ctrl+N from a terminal did not create a file inside the collapsed root");
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
        send(tree,WM_KEYDOWN,VK_F2);const auto edit=FindWindowExW(tree,nullptr,L"EDIT",nullptr);
        check(edit!=nullptr,"F2 did not open a filename editor");
        check(SetWindowTextW(edit,L"修正.py"),"Could not enter a Unicode filename");
        send(edit,WM_KEYDOWN,VK_RETURN);
        waitFor([&]{return fs::exists(fixture.root/L"修正.py")&&!fs::exists(fixture.root/L"a.txt");},"Rename did not move the file");
        waitLines(viewer,121);send(viewer,WM_KEYDOWN,VK_BACK);waitLines(viewer,120);
        check(visibleTabCount(app)==1,"Rename duplicated or closed the open tab");
        send(tree,WM_KEYDOWN,VK_F2);const auto cancel=FindWindowExW(tree,nullptr,L"EDIT",nullptr);
        check(cancel!=nullptr,"Second rename editor was missing");SetWindowTextW(cancel,L"cancelled.txt");send(cancel,WM_KEYDOWN,VK_ESCAPE);
        check(!fs::exists(fixture.root/L"cancelled.txt")&&fs::exists(fixture.root/L"修正.py"),"Escape committed a rename");
        send(tree,WM_KEYDOWN,VK_F2);const auto collision=FindWindowExW(tree,nullptr,L"EDIT",nullptr);SetWindowTextW(collision,L"b.txt");send(collision,WM_KEYDOWN,VK_RETURN);
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
void splitPaneTests(const fs::path& executable) {
    Fixture fixture; writeLines(fixture.root / L"a.txt", 120); writeLines(fixture.root / L"b.txt", 240);
    LONG divider = 0;
    {
        Application app(executable, fixture.root); const auto tree = app.tree();
        const auto left = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        const auto right = FindWindowExW(app.window, left, L"PoppyViewer", nullptr);
        waitFor([&] { return send(tree, treeGetCount) == 3; }, "Split fixture did not load");
        const auto first = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
        send(tree, treeSelect, first); waitLines(left, 120);
        send(tree, treeSelect, nextItem(tree, TreeRelation::Next, first)); waitLines(left, 240);
        const auto original = bounds(app.window, left);
        dragTab(app, 1, {original.left + (original.right-original.left)*3/4, original.top + 60});
        waitFor([&] { return IsWindowVisible(right); }, "Editor drag did not create a right group");
        waitLines(left, 120); waitLines(right, 240);
        const auto l = bounds(app.window,left), r = bounds(app.window,right);
        check(l.right < r.left && l.bottom == r.bottom, "Editors overlap or do not share a bottom edge");
        send(right, WM_CHAR, '\r'); waitLines(right, 241);
        clickTab(app, 0); waitLines(left, 120); clickTab(app, 1); waitLines(right, 241);
        send(right, WM_KEYDOWN, VK_BACK); waitLines(right, 240);
        const int x = (l.right+r.left)/2, y = l.top+40;
        send(app.window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        send(app.window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+30,y));
        send(app.window,WM_LBUTTONUP,0,MAKELPARAM(x+30,y)); divider=bounds(app.window,left).right;
        check(divider>l.right,"Editor divider did not move");
        const auto terminalPane=FindWindowExW(app.window,nullptr,L"PoppyTerminalPane",nullptr);
        RECT client{};GetClientRect(terminalPane,&client);const auto dpi=GetDpiForWindow(terminalPane);
        click(terminalPane,client.right-MulDiv(17,dpi,96),MulDiv(16,dpi,96));
        send(terminalPane,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(MulDiv(135,dpi,96),MulDiv(16,dpi,96)));
        send(terminalPane,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(client.right*3/4,client.bottom/2));
        send(terminalPane,WM_LBUTTONUP,0,MAKELPARAM(client.right*3/4,client.bottom/2));
        const auto terminalLeft=FindWindowExW(terminalPane,nullptr,L"PoppyTerminal",nullptr);
        const auto terminalRight=FindWindowExW(terminalPane,terminalLeft,L"PoppyTerminal",nullptr);
        check(IsWindowVisible(terminalLeft)&&IsWindowVisible(terminalRight),"Terminal drag did not show two sessions");
        check(bounds(terminalPane,terminalLeft).right<bounds(terminalPane,terminalRight).left,"Terminal sessions overlap");
    }
    {
        Application app(executable,fixture.root);app.tree();
        const auto left=FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr),right=FindWindowExW(app.window,left,L"PoppyViewer",nullptr);
        waitLines(left,120);waitLines(right,240);
        check(IsWindowVisible(right)&&bounds(app.window,left).right==divider,"Restart lost editor groups or divider");
        const auto pane=FindWindowExW(app.window,nullptr,L"PoppyTerminalPane",nullptr);
        const auto a=FindWindowExW(pane,nullptr,L"PoppyTerminal",nullptr),b=FindWindowExW(pane,a,L"PoppyTerminal",nullptr);
        check(IsWindowVisible(a)&&IsWindowVisible(b),"Restart lost terminal groups");
        const auto target=tabBounds(app,0);dragTab(app,1,{target.right-1,(target.top+target.bottom)/2});
        waitFor([&]{return !IsWindowVisible(right);},"Moving the last right tab did not collapse the split");
        waitLines(left,240);
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
    const auto terminal = FindWindowExW(app.window, nullptr, L"PoppyTerminalPane", nullptr);
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
    check(activityRect.left == 0 && activityRect.right <= treeRect.left && treeRect.right < viewerRect.left,
          "Activity bar, sidebar and editor are not arranged from left to right");
    check(treeRect.top > viewerRect.top, "Explorer is missing its sidebar heading");
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
    const int x = (treeRect.right + viewerRect.left) / 2, y = treeRect.top + 20;
    send(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    // Keep the button held while reversing direction, as in a live resize.
    for (const int offset : {delta, 2 * delta, -delta, 0, delta}) {
        send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + offset, y));
        const auto resizedTree = bounds(app.window, tree), resizedViewer = bounds(app.window, viewer);
        const auto resizedTerminal = bounds(app.window, terminal);
        check(resizedTree.right >= treeRect.right + offset - 1 && resizedTree.right <= treeRect.right + offset + 1,
              "Dragging the sidebar boundary did not resize the sidebar");
        check(resizedViewer.left - resizedTree.right == viewerRect.left - treeRect.right && resizedTree.left == treeRect.left,
              "Resizing moved the activity bar or overlapped the editor");
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
    check(!expanded(tree, directory) && send(tree, treeGetCount) == 123, "Collapsing the custom tree retained child nodes");
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
    const int row = static_cast<int>(send(tree, treeGetRow, directory)), height = static_cast<int>(send(tree, treeGetRowHeight));
    const int arrowX = MulDiv(27, GetDpiForWindow(app.window), 96), y = row * height + height / 2;
    click(tree, arrowX, y);
    waitFor([&] { return expanded(tree, directory) && nextItem(tree, TreeRelation::Child, directory); }, "Clicking the arrow did not expand the custom tree");
    click(tree, arrowX, y);
    check(!expanded(tree, directory) && !nextItem(tree, TreeRelation::Child, directory), "Clicking the arrow did not collapse and release children");
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
            expand(tree, nestedItem, false);
            check(!expanded(tree, nestedItem) && !nextItem(tree, TreeRelation::Child, nestedItem),
                  "Collapsing a directory did not release its children");
        }
        expand(tree, emptyItem);
        // Both enumerations use the same worker. Waiting for the next one
        // ensures the empty-directory result was applied before adding a file.
        expand(tree, nestedItem);
        waitFor([&] { return expanded(tree, nestedItem) && nextItem(tree, TreeRelation::Child, nestedItem); },
                "The directory worker did not finish enumerating the empty folder");
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
        click(app.window, px(520), px(22));
        check(IsWindowVisible(palette), "Find file did not open quick open");
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
    waitLines(viewer, 301);
    open(); type(L"missing-file"); send(palette, WM_KEYDOWN, VK_RETURN);
    check(IsWindowVisible(palette), "Enter opened a stale result while a new query was pending");
    send(palette, WM_KEYDOWN, VK_ESCAPE);
    check(!IsWindowVisible(palette) && scrollInfo(viewer).nMax == 301, "Escape changed the current document");
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
    // Replacing the selected match in the live buffer must exclude the old
    // disk contents, without saving the file or allocating a text snapshot.
    send(viewer, WM_CHAR, '!'); finished(0);
    send(button, BM_CLICK); type(L"!"); finished(1);
    send(input, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
    send(viewer, WM_CHAR, '\r'); waitLines(viewer, 301); finished(0);
    check(PostMessageW(input, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh project search");
    finished(0);
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
        waitFor([&] { return IsWindowVisible(changes) && send(changes, treeGetCount) == 8; }, "Default Git Tree did not load all untracked files");
        const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
        const auto view = [&](bool tree) {
            const auto rect = bounds(app.window, changes);
            click(app.window, rect.left + (rect.right - rect.left) * (tree ? 1 : 3) / 4, rect.top - px(14));
            check(send(changes, treeGetCount) == (tree ? 8 : 5), "Git layout toggle did not rebuild the hierarchy");
        };
        view(false); view(true);
        auto group = nextItem(changes, TreeRelation::Next, nextItem(changes, TreeRelation::Root));
        auto docs = nextItem(changes, TreeRelation::Child, group), src = nextItem(changes, TreeRelation::Next, docs);
        check(expanded(changes, docs) && expanded(changes, src), "Git folders did not start expanded");
        send(changes, treeSelect, docs);
        check(scrollInfo(viewer).nMax == 0, "Selecting a Git folder opened a file");
        send(changes, treeSelect, nextItem(changes, TreeRelation::Child, docs)); waitLines(viewer, 200);
        expand(changes, src, false);
        check(!expanded(changes, src), "A Git folder did not collapse");
        view(false); view(true);
        group = nextItem(changes, TreeRelation::Next, nextItem(changes, TreeRelation::Root));
        docs = nextItem(changes, TreeRelation::Child, group); src = nextItem(changes, TreeRelation::Next, docs);
        check(!expanded(changes, src), "Changing layouts forgot a collapsed Git folder");
        check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh Git changes");
        waitFor([&] { return nextItem(changes, TreeRelation::Next, nextItem(changes, TreeRelation::Root)) != group; }, "Git changes did not refresh");
        group = nextItem(changes, TreeRelation::Next, nextItem(changes, TreeRelation::Root));
        docs = nextItem(changes, TreeRelation::Child, group); src = nextItem(changes, TreeRelation::Next, docs);
        check(!expanded(changes, src), "Refreshing forgot a collapsed Git folder");
        expand(changes, src);
        const auto nested = nextItem(changes, TreeRelation::Child, src), file = nextItem(changes, TreeRelation::Child, nested);
        check(file != 0, "Git Tree lost a nested untracked file");
        send(changes, treeSelect, file); waitLines(viewer, 300);
        view(false);
        send(changes, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
        view(true);
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto changes = GetDlgItem(app.window, 4);
        waitFor([&] { return IsWindowVisible(changes) && send(changes, treeGetCount) == 8; }, "Restart lost the Git Tree layout");
        const auto rect = bounds(app.window, changes);
        click(app.window, rect.left + (rect.right - rect.left) * 3 / 4, rect.top - MulDiv(14, GetDpiForWindow(app.window), 96));
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto changes = GetDlgItem(app.window, 4);
        waitFor([&] { return IsWindowVisible(changes) && send(changes, treeGetCount) == 5; }, "Restart lost the Git List layout");
    }
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
        const auto first = nextItem(changes, TreeRelation::Root);
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
        const int y = static_cast<int>(send(changes, treeGetRow, item)) * height + height / 2;
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
    waitFor([&] { return badge(0) && send(changes, treeGetCount) == 2 && !IsWindowVisible(changes); }, "Clean repository did not hide the badge and change list");
    send(GetDlgItem(app.window, 2), BM_CLICK);
    writeLines(fixture.root / L"tracked.txt", 210);
    waitFor([&] { return badge(1) && send(changes, treeGetCount) == 3; }, "External edits did not update the hidden Source Control list and badge");
    send(button, BM_CLICK);
    waitFor([&] { return IsWindowVisible(changes); }, "Source Control did not show external edits");
    clickAction(firstFile(false));
    waitFor([&] { return nextItem(changes, TreeRelation::Child, nextItem(changes, TreeRelation::Root)) != 0; }, "Staging did not automatically update Source Control");
    clickAction(firstFile(true));
    waitFor([&] { return badge(1) && !firstFile(true) && firstFile(false) && send(changes, treeGetCount) == 3; }, "The minus icon did not move the file back to Changes");
    clickAction(firstFile(false));
    waitFor([&] { return firstFile(true) && !firstFile(false) && send(changes, treeGetCount) == 3; }, "The plus icon did not move the file to Staged Changes");
    writeLines(fixture.root / L"tracked.txt", 220);
    waitFor([&] { return badge(1) && send(changes, treeGetCount) == 4; }, "A partially staged file was not refreshed or was counted twice");
    clickAction(firstFile(false));
    waitFor([&] { return badge(1) && firstFile(true) && !firstFile(false) && send(changes, treeGetCount) == 3; }, "Staging a partially staged file did not update both groups");
    clickAction(firstFile(true));
    waitFor([&] { return badge(1) && !firstFile(true) && firstFile(false) && send(changes, treeGetCount) == 3; }, "Unstaging did not preserve the changed file and badge");
    runGit(L"reset --hard -q HEAD");
    waitFor([&] { return badge(0) && !IsWindowVisible(changes); }, "Reverting changes did not remove the badge");
    fs::create_directory(fixture.root / L"nested 日本語");
    writeFile(fixture.root / L"nested 日本語" / L"added.txt");
    waitFor([&] { return badge(1) && IsWindowVisible(changes) && send(changes, treeGetCount) == 4; }, "Creating an untracked file did not update Source Control");
    fs::remove(fixture.root / L"tracked.txt");
    waitFor([&] { return badge(2) && send(changes, treeGetCount) == 5; }, "Deleting a tracked file did not add to the pending file count");
    writeLines(fixture.root / L"tracked.txt", 200);
    waitFor([&] { return badge(1) && send(changes, treeGetCount) == 4; }, "Restoring a tracked file did not update the pending file count");
    clickAction(firstFile(false));
    waitFor([&] { return badge(1) && firstFile(true) && !firstFile(false); }, "The plus icon did not stage a nested untracked file");
    clickAction(firstFile(true));
    waitFor([&] { return badge(1) && !firstFile(true) && firstFile(false); }, "The minus icon did not preserve a nested untracked file");
    fs::rename(fixture.root / L"nested 日本語" / L"added.txt", fixture.root / L"moved.txt");
    waitFor([&] { return badge(1) && send(changes, treeGetCount) == 3; }, "Renaming an untracked file did not update Source Control");
    fs::remove(fixture.root / L"moved.txt");
    waitFor([&] { return badge(0) && !IsWindowVisible(changes); }, "Deleting the last untracked file did not clear Source Control");
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
    write(L"src/file.txt", "staged\n"); runGit(L"add .");
    write(L"src/file.txt", "working\n"); write(L"untracked.txt", "untracked\n");
    {
        Application app(executable, fixture.root); app.tree();
        const auto changes = GetDlgItem(app.window, 4), branches = GetDlgItem(app.window, 6);
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        send(GetDlgItem(app.window, 3), BM_CLICK);
        waitFor([&] { return IsWindowVisible(changes) && send(changes, treeGetCount) == 5; }, "Source Control did not separate partially staged changes");
        const auto staged = nextItem(changes, TreeRelation::Root), working = nextItem(changes, TreeRelation::Next, staged);
        check(children(changes, staged) == 1 && children(changes, working) == 2, "Staged and unstaged groups contain the wrong files");
        send(changes, treeSelect, staged); send(changes, WM_KEYDOWN, VK_F4); waitLines(viewer, 1);
        const auto rect = bounds(app.window, changes);
        const int height = static_cast<int>(send(changes, treeGetRowHeight));
        const int row = static_cast<int>(send(changes, treeGetRow, working));
        click(changes, rect.right - rect.left - MulDiv(18, GetDpiForWindow(app.window), 96), row * height + height / 2);
        waitLines(viewer, 4);
        // The group keeps its diff action and adds Stage All Changes beside it,
        // including when its file list is collapsed.
        expand(changes, working, false);
        const int stageX = rect.right - rect.left - MulDiv(46, GetDpiForWindow(app.window), 96);
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(stageX, row * height + height / 2));
        click(changes, stageX, row * height + height / 2);
        send(changes, WM_MOUSELEAVE);
        waitFor([&] {
            const auto first = nextItem(changes, TreeRelation::Root), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 2 && children(changes, second) == 0;
        }, "Stage All Changes did not stage partially staged and untracked files from a collapsed group");
        waitLines(viewer, 0);
        runGit(L"diff --quiet");
        const auto stagedAll = nextItem(changes, TreeRelation::Root);
        const int stagedRow = static_cast<int>(send(changes, treeGetRow, stagedAll));
        const int stagedY = stagedRow * height + height / 2;
        // Open All Changes remains the rightmost action on Staged Changes.
        click(changes, rect.right - rect.left - MulDiv(18, GetDpiForWindow(app.window), 96), stagedY);
        waitLines(viewer, 4);
        expand(changes, stagedAll, false);
        send(changes, WM_MOUSEMOVE, 0, MAKELPARAM(stageX, stagedY));
        click(changes, stageX, stagedY); send(changes, WM_MOUSELEAVE);
        waitFor([&] {
            const auto first = nextItem(changes, TreeRelation::Root), second = nextItem(changes, TreeRelation::Next, first);
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
            const auto first = nextItem(changes, TreeRelation::Root), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 1 && children(changes, second) == 2;
        }, "Discard fixture did not show partially staged and untracked changes");
        auto discardWorking = nextItem(changes, TreeRelation::Next, nextItem(changes, TreeRelation::Root));
        send(changes, treeSelect, discardWorking); send(changes, WM_KEYDOWN, VK_F4); waitLines(viewer, 4);
        discardGroup(app, changes, discardWorking, IDCANCEL);
        check(fs::exists(fixture.root / L"untracked.txt") && fs::file_size(fixture.root / L"src/file.txt") == 13,
              "Cancelling discard changed working files");
        waitLines(viewer, 4);
        discardWorking = nextItem(changes, TreeRelation::Next, nextItem(changes, TreeRelation::Root));
        discardGroup(app, changes, discardWorking, IDOK);
        waitFor([&] {
            const auto first = nextItem(changes, TreeRelation::Root), second = nextItem(changes, TreeRelation::Next, first);
            return children(changes, first) == 1 && children(changes, second) == 0 && !fs::exists(fixture.root / L"untracked.txt");
        }, "Discard did not preserve Staged Changes or clear Changes");
        waitLines(viewer, 0);
        runGit(L"diff --quiet");
        check(fs::file_size(fixture.root / L"src/file.txt") == 8 && fs::exists(fixture.root / L"ignored.txt"),
              "Discard did not restore the staged text or removed ignored files");
        GetWindowTextW(GetDlgItem(app.window, 3), badge, 128);
        check(std::wstring(badge) == L"Source Control (1)", "Discard did not update the badge");
        send(GetDlgItem(app.window, 5), BM_CLICK);
        waitFor([&] { return IsWindowVisible(branches) && children(branches, nextItem(branches, TreeRelation::Root)) == 51; }, "Branch did not load 50 commits and a load-more row");
        check(!IsWindowVisible(changes), "Branch did not hide Source Control");
        const auto branch = nextItem(branches, TreeRelation::Root), latest = nextItem(branches, TreeRelation::Child, branch);
        auto more = latest;
        for (int i = 0; i < 50; ++i) more = nextItem(branches, TreeRelation::Next, more);
        check(more != 0, "The 51st row is missing");
        send(branches, treeSelect, more);
        waitFor([&] { return children(branches, branch) == 52; }, "Clicking the 51st row did not append the remaining commits");
        auto rootCommit = latest;
        while (const auto next = nextItem(branches, TreeRelation::Next, rootCommit)) rootCommit = next;
        send(branches, treeSelect, latest);
        waitFor([&] { return children(branches, latest) == 2; }, "Opening a commit did not build its changed-file tree");
        const auto directory = nextItem(branches, TreeRelation::Child, latest), file = nextItem(branches, TreeRelation::Child, directory);
        check(file != 0, "Commit files are missing their folder hierarchy");
        send(branches, treeSelect, file); waitLines(viewer, 1);
        send(branches, treeSelect, latest); send(branches, WM_KEYDOWN, VK_F4); waitLines(viewer, 4);
        send(branches, treeSelect, rootCommit); send(branches, WM_KEYDOWN, VK_F4); waitLines(viewer, 5);
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto branches = GetDlgItem(app.window, 6), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        waitFor([&] { return IsWindowVisible(branches); }, "Restart did not restore the Branch page");
        waitLines(viewer, 5);
    }
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
        const auto changes = GetDlgItem(app.window, 4), branches = GetDlgItem(app.window, 6);
        const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
        send(GetDlgItem(app.window, 3), BM_CLICK);
        const auto roots = [&](HWND tree) { return children(tree, 0) == 2; };
        waitFor([&] { return IsWindowVisible(changes) && roots(changes); }, "Source Control did not discover sibling repositories");
        wchar_t title[128]{}; GetWindowTextW(GetDlgItem(app.window, 3), title, 128);
        check(std::wstring(title) == L"Source Control (4)", "The badge counted changes outside the opened folder");
        const auto group = [&](TreeItem repo, bool staged) { const auto first = nextItem(changes, TreeRelation::Child, repo); return staged ? first : nextItem(changes, TreeRelation::Next, first); };
        auto clientRepo = nextItem(changes, TreeRelation::Root), serverRepo = nextItem(changes, TreeRelation::Next, clientRepo);
        check(children(changes, clientRepo) == 2 && children(changes, serverRepo) == 2, "Repositories did not get independent change groups");
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
        const int x = changesRect.right - MulDiv(46, GetDpiForWindow(app.window), 96), y = row * height + height / 2;
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
        const int unstageY = unstageRow * height + height / 2;
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
        send(GetDlgItem(app.window, 5), BM_CLICK);
        waitFor([&] { return IsWindowVisible(branches) && roots(branches); }, "Branch did not discover sibling repositories");
        const auto clientBranchRepo = nextItem(branches, TreeRelation::Root), serverBranchRepo = nextItem(branches, TreeRelation::Next, clientBranchRepo);
        for (const auto repo : {clientBranchRepo, serverBranchRepo}) {
            const auto branch = nextItem(branches, TreeRelation::Child, repo);
            waitFor([&] { return children(branches, branch) == 2; }, "A repository's main branch did not load its history");
            const auto latest = nextItem(branches, TreeRelation::Child, branch), commit = nextItem(branches, TreeRelation::Next, latest);
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
    if (Process32FirstW(snapshot, &entry)) do { if (entry.th32ParentProcessID == parent && _wcsicmp(entry.szExeFile, L"powershell.exe") == 0) result.insert(entry.th32ProcessID); } while (Process32NextW(snapshot, &entry));
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
    send(treeA, treeSelect, nextItem(treeA, TreeRelation::Child, nextItem(treeA, TreeRelation::Root))); waitLines(viewerA, 200);
    send(viewerA, WM_VSCROLL, SB_PAGEDOWN); const int top = scrollInfo(viewerA).nPos;
    const auto rootA = nextItem(treeA, TreeRelation::Root);
    const auto open = [&](const fs::path& path) {
        const auto text = path.native(); COPYDATASTRUCT data{0x50505901, static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)), const_cast<wchar_t*>(text.c_str())};
        check(send(app.host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)), "Could not add a second project");
        app.tree();
    };
    const auto select = [&](size_t index) { const UINT dpi=GetDpiForWindow(app.host);click(app.host,MulDiv(60,dpi,96),MulDiv(44+static_cast<int>(index)*48+24,dpi,96));app.tree(); };
    open(b.root); const auto surfaceB = app.window; const auto treeB = app.tree();
    const auto viewerB = FindWindowExW(surfaceB, nullptr, L"PoppyViewer", nullptr);
    waitFor([&] { return nextItem(treeB, TreeRelation::Child, nextItem(treeB, TreeRelation::Root)) != 0; }, "Project B did not load");
    send(treeB, treeSelect, nextItem(treeB, TreeRelation::Child, nextItem(treeB, TreeRelation::Root))); waitLines(viewerB, 300);
    open(c.root); check(app.window!=surfaceA && app.window!=surfaceB,"Projects reused a surface");
    select(0); check(app.window==surfaceA && nextItem(treeA,TreeRelation::Root)==rootA,"Switching rebuilt the explorer");
    check(scrollInfo(viewerA).nMax==200 && scrollInfo(viewerA).nPos==top,"A-B-A lost the document or its view position");
    const auto terminals = FindWindowExW(surfaceA,nullptr,L"PoppyTerminalPane",nullptr);check(terminals!=nullptr,"Project has no terminal pane");
    HWND shell = FindWindowExW(terminals,nullptr,L"PoppyTerminal",nullptr);check(shell!=nullptr,"Project has no shell");
    const auto before = childProcesses(app.process.dwProcessId);
    click(shell,10,10); // Start only this project's shell.
    DWORD shellPid = 0;
    waitFor([&] { for (const auto child : childProcesses(app.process.dwProcessId)) if (!before.count(child)) shellPid = child; return shellPid != 0; }, "Shell process did not start");
    const auto shellProcess = OpenProcess(SYNCHRONIZE, FALSE, shellPid); check(shellProcess != nullptr, "Cannot inspect the running shell");
    const auto px=[dpi=GetDpiForWindow(terminals)](int v){return MulDiv(v,dpi,96);};
    RECT client{};GetClientRect(terminals,&client);click(terminals,client.right-px(12),px(16));
    check(FindWindowExW(terminals,shell,L"PoppyTerminal",nullptr)!=nullptr,"Adding a shell replaced the first session");
    select(1);
    writeFile(a.root / L"background.txt");
    waitFor([&] { return send(treeA, treeGetCount) == 3; }, "An inactive project's directory changes were not applied");
    check(scrollInfo(viewerB).nMax == 300 && app.window == surfaceB, "An inactive result changed the visible project");
    select(0);check(IsWindow(shell) && WaitForSingleObject(shellProcess, 0) == WAIT_TIMEOUT,"Switching destroyed a running terminal");
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); double maximum = 0;
    for (int i = 0; i < 6; ++i) { LARGE_INTEGER start{}, end{}; QueryPerformanceCounter(&start); select(i % 2); QueryPerformanceCounter(&end); maximum = std::max(maximum, (end.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart); }
    std::cout << "Retained project switch maximum: " << maximum << " ms\n";
    send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(60),px(68)));
    send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(60),px(44+3*48-1)));
    send(app.host,WM_LBUTTONUP,0,MAKELPARAM(px(60),px(44+3*48-1)));
    check(app.tree()==treeA && app.window==surfaceA && scrollInfo(viewerA).nPos==top &&
          IsWindow(shell) && WaitForSingleObject(shellProcess,0)==WAIT_TIMEOUT,"Reordering a project lost its document or running terminal");
    select(0);check(app.window==surfaceB,"Dragging a project did not change the row order");
    send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(60),px(44+2*48+24)));
    send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(60),px(45)));
    send(app.host,WM_LBUTTONUP,0,MAKELPARAM(px(60),px(45)));
    select(0);check(app.window==surfaceA,"Dragging a project back to the first row failed");
    open(a.root);check(app.window==surfaceA,"Reopening a registered path duplicated the project");
    const auto oldB=surfaceB;const auto dpi=GetDpiForWindow(app.host);click(app.host,MulDiv(60,dpi,96),MulDiv(44+48+24,dpi,96),true);
    check(!IsWindow(oldB)&&IsWindow(surfaceA)&&IsWindow(shell) && WaitForSingleObject(shellProcess, 0) == WAIT_TIMEOUT,"Closing an inactive project affected another session");
    CloseHandle(shellProcess);
    }
    {
        Application restored(executable); const auto tree = restored.tree();
        waitLines(FindWindowExW(restored.window, nullptr, L"PoppyViewer", nullptr), 200);
        size_t count = 0; for (HWND project = nullptr; (project = FindWindowExW(restored.host, project, L"PoppyProject", nullptr));) ++count;
        check(count == 2 && nextItem(tree, TreeRelation::Root), "Restart lost the open list or reopened a closed project");
        const auto pane = FindWindowExW(restored.window, nullptr, L"PoppyTerminalPane", nullptr);
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
        // shortcut. This also verifies Ctrl+R from editor/tree/terminal focus.
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
        const auto pane = FindWindowExW(surfaceB, nullptr, L"PoppyTerminalPane", nullptr);
        const auto terminal = FindWindowExW(pane, nullptr, L"PoppyTerminal", nullptr);
        check(terminal != nullptr, "History test terminal is missing");
        palette = show(app, terminal); send(palette, WM_KEYDOWN, VK_DOWN); send(palette, WM_KEYDOWN, VK_RETURN);
        app.tree(); check(app.window == surfaceA && count(app.host) == 2, "History duplicated an already open project");
        click(app.host, px(60), px(44+48+24), true);
        check(!IsWindow(surfaceB) && count(app.host) == 1, "History test could not close project B");
        palette = show(app, FindWindowExW(surfaceA, nullptr, L"PoppyViewer", nullptr));
        type(palette, L"右 日本語"); click(palette, px(24), px(108));
        app.tree(); waitLines(FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr), 300);
        check(count(app.host) == 2 && !IsWindowVisible(palette), "History could not reopen a closed project by path");
        // Close both, leaving a saved workspace with no open project.
        click(app.host, px(60), px(44+48+24), true); click(app.host, px(60), px(68), true);
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
            return app.window;
        };
        const auto open=[&](const fs::path& path,int lines){
            const auto text=path.native();COPYDATASTRUCT data{0x50505901,static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)),const_cast<wchar_t*>(text.c_str())};
            check(send(app.host,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data)),"Could not add a drag test project");
            return loadFile(lines);
        };
        const auto surfaceA=loadFile(200),surfaceB=open(b.root,300),surfaceC=open(c.root,400);
        const auto px=[dpi=GetDpiForWindow(app.host)](int value){return MulDiv(value,dpi,96);};
        const auto point=[&](int row,int offset){return POINT{px(60),px(44+row*48+offset)};};
        const auto order=[&](std::initializer_list<HWND> expected){
            int index=0;for(const auto surface:expected){const auto p=point(index++,24);click(app.host,p.x,p.y);app.tree();check(app.window==surface,"Project drag produced the wrong order");}
        };
        const auto begin=[&](int from,POINT target){
            const auto p=point(from,24);send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(p.x,p.y));
            send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(target.x,target.y));
        };
        const auto drop=[&](POINT target){send(app.host,WM_LBUTTONUP,0,MAKELPARAM(target.x,target.y));};
        begin(0,point(2,47));drop(point(2,47));app.tree();
        check(app.window==surfaceA,"Reordering changed the selected project");order({surfaceB,surfaceC,surfaceA});
        begin(2,point(0,1));drop(point(0,1));order({surfaceA,surfaceB,surfaceC});
        begin(0,point(0,47));drop(point(0,47));order({surfaceA,surfaceB,surfaceC});
        auto tiny=point(0,24);++tiny.x;begin(0,tiny);drop(tiny);order({surfaceA,surfaceB,surfaceC});
        for(const auto cancel:{WM_CANCELMODE,WM_CAPTURECHANGED,WM_KEYDOWN}){
            begin(0,point(2,47));send(app.host,cancel,cancel==WM_KEYDOWN?VK_ESCAPE:0);drop(point(2,47));order({surfaceA,surfaceB,surfaceC});
        }
        RECT client{};GetClientRect(app.host,&client);
        for(const POINT outside: {POINT{px(300),px(80)},POINT{px(60),px(1)},POINT{px(60),client.bottom-px(1)}}){
            begin(0,point(2,47));drop(outside);order({surfaceA,surfaceB,surfaceC});
        }
        // Escape must work while a project's editor owns keyboard focus.
        begin(0,point(2,47));
        check(PostMessageW(FindWindowExW(surfaceA,nullptr,L"PoppyViewer",nullptr),WM_KEYDOWN,VK_ESCAPE,0)!=FALSE,"Could not post drag cancellation");
        waitFor([&]{GUITHREADINFO info{};info.cbSize=sizeof(info);check(GetGUIThreadInfo(app.process.dwThreadId,&info)!=FALSE,"Could not inspect project capture");return info.hwndCapture==nullptr;},"Escape did not release project drag capture");
        drop(point(2,47));order({surfaceA,surfaceB,surfaceC});
        // Blank space after the last project accepts an append operation.
        begin(1,point(4,24));drop(point(4,24));order({surfaceA,surfaceC,surfaceB});
    }
    {
        Application app(executable);app.tree();
        waitLines(FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr),300);
        const auto px=[dpi=GetDpiForWindow(app.host)](int value){return MulDiv(value,dpi,96);};
        int index=0;for(const int lines:{200,400,300}){
            click(app.host,px(60),px(44+index++*48+24));app.tree();
            waitLines(FindWindowExW(app.window,nullptr,L"PoppyViewer",nullptr),lines);
        }
    }
}
void scrollingProjectDragTests(const fs::path& executable) {
    Fixture fixture;HWND surfaces[12]{};
    for(int i=0;i<12;++i){const auto path=fixture.root/std::to_wstring(i);fs::create_directory(path);writeFile(path/L"test.txt");}
    Application app(executable,fixture.root/L"0");app.tree();surfaces[0]=app.window;
    for(int i=1;i<12;++i){
        const auto text=(fixture.root/std::to_wstring(i)).native();
        COPYDATASTRUCT data{0x50505901,static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)),const_cast<wchar_t*>(text.c_str())};
        check(send(app.host,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data)),"Could not add a scrolling project");app.tree();surfaces[i]=app.window;
    }
    const auto px=[dpi=GetDpiForWindow(app.host)](int value){return MulDiv(value,dpi,96);};
    check(SetWindowPos(app.host,nullptr,0,0,px(1000),px(480),SWP_NOMOVE|SWP_NOZORDER)!=FALSE,"Could not resize the project drag test");
    RECT client{};GetClientRect(app.host,&client);POINT wheel{px(60),px(68)};ClientToScreen(app.host,&wheel);
    send(app.host,WM_MOUSEWHEEL,MAKEWPARAM(0,12000),MAKELPARAM(wheel.x,wheel.y));
    const POINT bottom{px(60),client.bottom-px(45)};
    send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(60),px(68)));
    send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bottom.x,bottom.y));
    for(int i=0;i<20;++i)send(app.host,WM_TIMER,1);
    send(app.host,WM_LBUTTONUP,0,MAKELPARAM(bottom.x,bottom.y));app.tree();
    check(app.window==surfaces[0],"Scrolling a dragged project lost its selection");
    send(app.host,WM_MOUSEWHEEL,MAKEWPARAM(0,12000),MAKELPARAM(wheel.x,wheel.y));
    click(app.host,px(60),px(68));app.tree();check(app.window==surfaces[1],"Auto-scroll did not move the first project to the end");
    send(app.host,WM_MOUSEWHEEL,MAKEWPARAM(0,-12000),MAKELPARAM(wheel.x,wheel.y));
    const int page=(client.bottom-px(88))/px(48);
    const int lastY=px(44)+(page-1)*px(48)+px(24);
    send(app.host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px(60),lastY));
    send(app.host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(px(60),px(45)));
    for(int i=0;i<20;++i)send(app.host,WM_TIMER,1);
    send(app.host,WM_LBUTTONUP,0,MAKELPARAM(px(60),px(45)));
    click(app.host,px(60),px(68));app.tree();check(app.window==surfaces[0],"Auto-scroll did not move the last project to the start");
}
void terminalBackspaceTests(const fs::path& executable) {
    Fixture fixture;
    writeFile(fixture.root / L"test.txt");
    Application app(executable, fixture.root);
    app.tree();
    const auto pane = FindWindowExW(app.window, nullptr, L"PoppyTerminalPane", nullptr);
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
        send(tree, treeSelect, first); waitLines(viewer, 200);
        for (int i = 0; i < 12; ++i) { send(viewer, WM_VSCROLL, SB_LINEDOWN); send(viewer, WM_HSCROLL, SB_LINEDOWN); }
        firstTop = scrollInfo(viewer).nPos; firstLeft = scrollInfo(viewer, SB_HORZ).nPos;
        send(tree, treeSelect, second); waitLines(viewer, 300);
        send(viewer, WM_VSCROLL, SB_PAGEDOWN); secondTop = scrollInfo(viewer).nPos;
        send(tree, treeSelect, third); waitLines(viewer, 100);
        send(tree, treeSelect, second); waitLines(viewer, 300);
        const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
        click(app.window, px(410), px(22));
        const auto pane = bounds(app.window, viewer);
        const int splitter = pane.left - px(3), y = pane.top + px(50);
        send(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(splitter, y));
        send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(splitter + px(60), y));
        send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(splitter + px(60), y));
        sidebarEdge = bounds(app.window, viewer).left;
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
        check(!IsWindowVisible(FindWindowExW(app.window, nullptr, L"PoppyTerminalPane", nullptr)), "Restart lost terminal visibility");
        check(!IsWindowVisible(tree), "Restart lost the active sidebar");
        const auto pane = bounds(app.window, viewer);
        check(pane.left == sidebarEdge, "Restart lost sidebar width");
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
        check(argc == 2, "Pass the path to Poppy.exe");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        for (const auto test : {folderTests, sidebarTests, treeInteractionTests, tabTests, pinnedTabTests,
                tabDragTests, wrappingTabDragTests, wrappingTabTests, renameTests, createItemTests, newFileShortcutTests, pasteImageTests, splitPaneTests, quickOpenTests, fullSearchTests, gitSidebarTests, gitAutoRefreshTests, branchSidebarTests, multiRepositoryTests, sessionTests, multiProjectTests, recentProjectTests, projectDragTests, scrollingProjectDragTests, terminalBackspaceTests}) {
            Fixture appData; AppDataEnvironment environment(appData.root);
            test(fs::absolute(argv[1]));
        }
        std::cout << "Folder, sidebar, Git layouts, tab, quick open, session and terminal input tests passed.\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
