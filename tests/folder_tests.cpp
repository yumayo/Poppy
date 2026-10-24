#include <windows.h>
#include "tree_messages.h"
#include "activity_button.h"
#include <filesystem>
#include <fstream>
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
    fs::path root;
    Fixture() : root(fs::temp_directory_path() / (L"Poppy folder test 日本語 " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()))) {
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
    HWND window = nullptr;
    Application(const fs::path& executable, const fs::path& folder = {}) {
        std::wstring command = L"\"" + executable.native() + L"\"";
        if (!folder.empty()) command += L" \"" + folder.native() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE,
              "Could not launch Poppy");
        CloseHandle(process.hThread); process.hThread = nullptr;
    }
    ~Application() {
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
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
                app.window = candidate; return FALSE;
            }, reinterpret_cast<LPARAM>(this));
            if (window) result = FindWindowExW(window, nullptr, treeClassName, L"Files");
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
    click(app.window, pane.left + px(180 + 166), tabY);
    check(scrollInfo(viewer).nMax == 220 && scrollInfo(viewer).nPos == firstTop, "Closing an inactive tab changed the active document");
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top && scrollInfo(viewer).nMax == 0, "Closing the last tab left a duplicate tab or document");
    // Reopen, then close a newly loading tab and its neighbor. Older results
    // must not repopulate the viewer or replace the next selected document.
    send(tree, treeSelect, static_cast<WPARAM>(first)); waitLines(viewer, 220);
    send(tree, treeSelect, static_cast<WPARAM>(second));
    click(app.window, pane.left + px(180 + 30), tabY, true);
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top, "Closing tabs during loading did not clear the tab strip");
    // b.txt is still selected in the tree, so reopening also exercises Enter.
    send(tree, WM_KEYDOWN, VK_RETURN); waitLines(viewer, 300);
    click(app.window, pane.left + px(30), tabY, true);
    check(bounds(app.window, viewer).top == empty.top && scrollInfo(viewer).nMax == 0, "A stale load restored a closed tab");
}
void overflowingTabTests(const fs::path& executable) {
    Fixture fixture;
    for (int i = 0; i < 8; ++i) writeLines(fixture.root / (L"tab-" + std::to_wstring(i) + L".txt"), 100 + i);
    Application app(executable, fixture.root);
    const auto tree = app.tree(), viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    check(viewer != nullptr, "The viewer is missing");
    const UINT dpi = GetDpiForWindow(app.window);
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    check(SetWindowPos(app.window, nullptr, 0, 0, px(820), px(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not resize the tab test window");
    waitFor([&] { return send(tree, treeGetCount) == 9; }, "The overflow test files did not appear");
    auto item = nextItem(tree, TreeRelation::Child, nextItem(tree, TreeRelation::Root));
    for (int i = 0; i < 8; ++i) {
        check(item != 0, "A tab test tree item is missing");
        send(tree, treeSelect, static_cast<WPARAM>(item)); waitLines(viewer, 100 + i);
        item = nextItem(tree, TreeRelation::Next, item);
    }
    const auto pane = bounds(app.window, viewer); const int tabY = pane.top - px(17);
    click(app.window, pane.right - px(42), tabY);
    check(scrollInfo(viewer).nMax == 107, "Scrolling the tab strip changed the active file");
    click(app.window, pane.left + px(30), tabY);
    waitFor([&] { const int lines = scrollInfo(viewer).nMax; return lines >= 100 && lines < 107; }, "The previous tab button did not reveal older tabs");
    for (int i = 0; i < 8; ++i) click(app.window, pane.right - px(14), tabY);
    click(app.window, pane.left + px(180 + 30), tabY); waitLines(viewer, 107);
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
        check(!IsWindowVisible(FindWindowExW(app.window, nullptr, L"PoppyTerminal", nullptr)), "Restart lost terminal visibility");
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
        Fixture appData;
        AppDataEnvironment environment(appData.root);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        folderTests(fs::absolute(argv[1]));
        sidebarTests(fs::absolute(argv[1]));
        treeInteractionTests(fs::absolute(argv[1]));
        tabTests(fs::absolute(argv[1]));
        overflowingTabTests(fs::absolute(argv[1]));
        sessionTests(fs::absolute(argv[1]));
        std::cout << "Folder, sidebar, tab and session tests passed.\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
