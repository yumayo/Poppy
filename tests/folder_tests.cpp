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
    for (int i = 0; i < lines; ++i) file << i << ' ' << std::string(240, 'x') << '\n';
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
    check(firstTop > 0 && firstLeft > 0, "The test document did not scroll");
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
        check(firstTop > 0 && firstLeft > 0, "The dragged document did not scroll");
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
void sidebarTests(const fs::path& executable) {
    Fixture fixture;
    const auto folder = fixture.root / L"nested";
    fs::create_directory(folder); writeFile(folder / L"file.txt");
    Application app(executable, fixture.root);
    const auto tree = app.tree();
    const auto explorer = FindWindowExW(app.window, nullptr, activityButtonClassName, L"Explorer");
    const auto git = FindWindowExW(app.window, nullptr, activityButtonClassName, L"Source Control");
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
        waitFor([&] { return IsWindowVisible(changes) && send(changes, treeGetCount) == 3; }, "Git List did not load all untracked files");
        const auto px = [dpi = GetDpiForWindow(app.window)](int value) { return MulDiv(value, dpi, 96); };
        const auto view = [&](bool tree) {
            const auto rect = bounds(app.window, changes);
            click(app.window, rect.left + (rect.right - rect.left) * (tree ? 1 : 3) / 4, rect.top - px(14));
            check(send(changes, treeGetCount) == (tree ? 6 : 3), "Git layout toggle did not rebuild the hierarchy");
        };
        view(true);
        auto docs = nextItem(changes, TreeRelation::Root), src = nextItem(changes, TreeRelation::Next, docs);
        check(expanded(changes, docs) && expanded(changes, src), "Git folders did not start expanded");
        send(changes, treeSelect, docs);
        check(scrollInfo(viewer).nMax == 0, "Selecting a Git folder opened a file");
        send(changes, treeSelect, nextItem(changes, TreeRelation::Child, docs)); waitLines(viewer, 200);
        expand(changes, src, false);
        check(!expanded(changes, src), "A Git folder did not collapse");
        view(false); view(true);
        docs = nextItem(changes, TreeRelation::Root); src = nextItem(changes, TreeRelation::Next, docs);
        check(!expanded(changes, src), "Changing layouts forgot a collapsed Git folder");
        check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh Git changes");
        waitFor([&] { return nextItem(changes, TreeRelation::Root) != docs; }, "Git changes did not refresh");
        docs = nextItem(changes, TreeRelation::Root); src = nextItem(changes, TreeRelation::Next, docs);
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
        waitFor([&] { return IsWindowVisible(changes) && send(changes, treeGetCount) == 6; }, "Restart lost the Git Tree layout");
        const auto rect = bounds(app.window, changes);
        click(app.window, rect.left + (rect.right - rect.left) * 3 / 4, rect.top - MulDiv(14, GetDpiForWindow(app.window), 96));
    }
    {
        Application app(executable, fixture.root); app.tree();
        const auto changes = GetDlgItem(app.window, 4);
        waitFor([&] { return IsWindowVisible(changes) && send(changes, treeGetCount) == 3; }, "Restart lost the Git List layout");
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
                tabDragTests, wrappingTabDragTests, wrappingTabTests, quickOpenTests, gitSidebarTests, sessionTests, multiProjectTests}) {
            Fixture appData; AppDataEnvironment environment(appData.root);
            test(fs::absolute(argv[1]));
        }
        std::cout << "Folder, sidebar, Git layouts, tab, quick open and session tests passed.\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
