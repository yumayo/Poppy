#include <windows.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
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
HTREEITEM nextItem(HWND tree, UINT relation, HTREEITEM item = nullptr) {
    return reinterpret_cast<HTREEITEM>(send(tree, TVM_GETNEXTITEM, relation, reinterpret_cast<LPARAM>(item)));
}
bool expanded(HWND tree, HTREEITEM item) {
    return (send(tree, TVM_GETITEMSTATE, reinterpret_cast<WPARAM>(item), TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
}
void expand(HWND tree, HTREEITEM item, bool value = true) {
    // Exercise user expansion: TVM_EXPAND can suppress repeat notifications
    // once TVIS_EXPANDEDONCE is set. The first key press may defer expansion.
    send(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(item));
    send(tree, WM_KEYDOWN, value ? VK_RIGHT : VK_LEFT);
}
struct Fixture {
    fs::path root;
    Fixture() : root(fs::temp_directory_path() / (L"Poppy folder test 日本語 " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()))) {
        check(fs::create_directory(root), "Could not create the test workspace");
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
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
    Application(const fs::path& executable, const fs::path& folder) {
        std::wstring command = L"\"" + executable.native() + L"\" \"" + folder.native() + L"\"";
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
            if (window) result = FindWindowExW(window, nullptr, WC_TREEVIEWW, L"Files");
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
    HTREEITEM first = nullptr, second = nullptr;
    waitFor([&] {
        first = nextItem(tree, TVGN_CHILD, nextItem(tree, TVGN_ROOT));
        second = nextItem(tree, TVGN_NEXT, first);
        return first && second;
    }, "The tab test files did not appear");
    const UINT dpi = GetDpiForWindow(app.window);
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const auto empty = bounds(app.window, viewer);
    send(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(first)); waitLines(viewer, 200);
    const auto pane = bounds(app.window, viewer);
    check(pane.top == empty.top + px(34), "Opening a file did not make room for tabs");
    for (int i = 0; i < 12; ++i) { send(viewer, WM_VSCROLL, SB_LINEDOWN); send(viewer, WM_HSCROLL, SB_LINEDOWN); }
    const auto firstTop = scrollInfo(viewer).nPos, firstLeft = scrollInfo(viewer, SB_HORZ).nPos;
    check(firstTop > 0 && firstLeft > 0, "The test document did not scroll");
    send(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(second)); waitLines(viewer, 300);
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
    send(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(first)); waitLines(viewer, 220);
    send(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(second));
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
    waitFor([&] { return send(tree, TVM_GETCOUNT) == 9; }, "The overflow test files did not appear");
    auto item = nextItem(tree, TVGN_CHILD, nextItem(tree, TVGN_ROOT));
    for (int i = 0; i < 8; ++i) {
        check(item != nullptr, "A tab test tree item is missing");
        send(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(item)); waitLines(viewer, 100 + i);
        item = nextItem(tree, TVGN_NEXT, item);
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
    const auto explorer = FindWindowExW(app.window, nullptr, L"BUTTON", L"Explorer");
    const auto git = FindWindowExW(app.window, nullptr, L"BUTTON", L"Source Control");
    const auto viewer = FindWindowExW(app.window, nullptr, L"PoppyViewer", nullptr);
    check(explorer && git && viewer, "Activity buttons or editor pane are missing");
    HTREEITEM nested = nullptr;
    waitFor([&] {
        nested = nextItem(tree, TVGN_CHILD, nextItem(tree, TVGN_ROOT));
        return nested != nullptr;
    }, "Explorer did not load its folder");
    expand(tree, nested);
    waitFor([&] { return expanded(tree, nested) && nextItem(tree, TVGN_CHILD, nested); }, "Explorer did not expand its folder");
    const UINT dpi = GetDpiForWindow(app.window);
    check(SetWindowPos(app.window, nullptr, 0, 0, MulDiv(1180, dpi, 96), MulDiv(820, dpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "Could not size the test window for its DPI");
    const auto activityRect = bounds(app.window, explorer), treeRect = bounds(app.window, tree), viewerRect = bounds(app.window, viewer);
    check(activityRect.left == 0 && activityRect.right <= treeRect.left && treeRect.right < viewerRect.left,
          "Activity bar, sidebar and editor are not arranged from left to right");
    check(treeRect.top > viewerRect.top, "Explorer is missing its sidebar heading");
    send(git, BM_CLICK);
    check(!IsWindowVisible(tree), "Git activity did not hide Explorer");
    const auto gitViewer = bounds(app.window, viewer);
    check(gitViewer.left == viewerRect.left && gitViewer.top == viewerRect.top,
          "Switching sidebars moved the editor");
    // Refresh while Explorer is hidden: WM_SETREDRAW must not make it visible.
    writeFile(fixture.root / L"added.txt");
    check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh the hidden Explorer");
    waitFor([&] { return send(tree, TVM_GETCOUNT) == 4; }, "Hidden Explorer did not refresh");
    check(!IsWindowVisible(tree), "Refreshing Explorer covered the Git sidebar");
    send(explorer, BM_CLICK);
    check(IsWindowVisible(tree) && expanded(tree, nested), "Switching back lost Explorer's expansion state");

    const int delta = MulDiv(48, dpi, 96);
    const int x = (treeRect.right + viewerRect.left) / 2, y = treeRect.top + 20;
    send(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    send(app.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + delta, y));
    send(app.window, WM_LBUTTONUP, 0, MAKELPARAM(x + delta, y));
    const auto resizedTree = bounds(app.window, tree), resizedViewer = bounds(app.window, viewer);
    check(resizedTree.right >= treeRect.right + delta - 1 && resizedTree.right <= treeRect.right + delta + 1,
          "Dragging the sidebar boundary did not resize the sidebar");
    check(resizedViewer.left - resizedTree.right == viewerRect.left - treeRect.right && resizedTree.left == treeRect.left,
          "Resizing moved the activity bar or overlapped the editor");
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
        HTREEITEM root = nullptr;
        waitFor([&] {
            root = nextItem(tree, TVGN_ROOT);
            return root && expanded(tree, root) && nextItem(tree, TVGN_CHILD, root);
        }, "Opening a workspace did not display its files");
        const auto emptyItem = nextItem(tree, TVGN_CHILD, root);
        const auto nestedItem = nextItem(tree, TVGN_NEXT, emptyItem);
        check(nestedItem && !nextItem(tree, TVGN_NEXT, nestedItem), "Workspace should contain two directories");
        for (int i = 0; i < 2; ++i) {
            expand(tree, nestedItem);
            waitFor([&] { return expanded(tree, nestedItem) && nextItem(tree, TVGN_CHILD, nestedItem); },
                    "Expanding a directory did not display its files");
            expand(tree, nestedItem, false);
            check(!expanded(tree, nestedItem) && !nextItem(tree, TVGN_CHILD, nestedItem),
                  "Collapsing a directory did not release its children");
        }
        expand(tree, emptyItem);
        // Both enumerations use the same worker. Waiting for the next one
        // ensures the empty-directory result was applied before adding a file.
        expand(tree, nestedItem);
        waitFor([&] { return expanded(tree, nestedItem) && nextItem(tree, TVGN_CHILD, nestedItem); },
                "The directory worker did not finish enumerating the empty folder");
        writeFile(empty / L"new file.txt");
        check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not request a refresh");
        waitFor([&] { return expanded(tree, emptyItem) && nextItem(tree, TVGN_CHILD, emptyItem); },
                "A file added to an empty directory did not appear");
    }
    {
        Fixture emptyRoot;
        Application app(executable, emptyRoot.root);
        const auto tree = app.tree();
        HTREEITEM root = nullptr;
        waitFor([&] { root = nextItem(tree, TVGN_ROOT); return root != nullptr; }, "Empty workspace has no root item");
        writeFile(emptyRoot.root / L"first file.txt");
        check(PostMessageW(app.window, WM_KEYDOWN, VK_F5, 0) != FALSE, "Could not refresh the empty workspace");
        waitFor([&] { return expanded(tree, root) && nextItem(tree, TVGN_CHILD, root); },
                "A file added to an empty workspace did not appear");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "Pass the path to Poppy.exe");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        folderTests(fs::absolute(argv[1]));
        sidebarTests(fs::absolute(argv[1]));
        tabTests(fs::absolute(argv[1]));
        overflowingTabTests(fs::absolute(argv[1]));
        std::cout << "Folder, sidebar and tab tests passed.\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
