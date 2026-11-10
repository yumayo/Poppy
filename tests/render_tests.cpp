#include "scrollbars.h"
#include "quick_open.h"
#include "viewer.h"
#include "file_drop.h"
#include "terminal.h"
#include "terminal_profile.h"
#include "status_bar.h"
#include "tree.h"
#include "git_commit_pane.h"
#include "shutdown_dialog.h"
#include "error_popup.h"
#include <commctrl.h>
#include <imm.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void editorBodyFileDrops() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Editor file drop test", WS_POPUP | WS_CLIPCHILDREN,
        100, 100, 640, 400, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the editor file drop owner");
    auto* target = new FileDropTarget({}, {}, {});
    Viewer viewer, split;
    struct Owner {
        HWND window; FileDropTarget* target;
        ~Owner() { DestroyWindow(window); target->Release(); }
    } owner{parent, target};
    const auto registered = [&](HWND window) {
        const auto result = RegisterDragDrop(window, target);
        if (SUCCEEDED(result)) RevokeDragDrop(window);
        check(result == DRAGDROP_E_ALREADYREGISTERED, "The editor body has no registered file drop target");
    };
    viewer.create(parent, instance); viewer.setFileDropTarget(target);
    MoveWindow(viewer.window(), 0, 34, 320, 366, FALSE);
    registered(viewer.window()); // Empty editor, without a file selected.
    viewer.setFileDropTarget(target); registered(viewer.window());
    auto buffer = std::make_shared<EditBuffer>(); buffer->document.text = "name,value\nfirst,123\n";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"C:\\outside\\sample.csv"); registered(viewer.window());
    auto before = std::make_shared<Document>(); before->text = "name,value\n"; indexDocument(*before);
    viewer.editDiff(buffer, before, L"C:\\outside\\sample.csv"); registered(viewer.window());
    auto image = std::make_shared<ImagePreview>();
    image->width = image->height = 1; image->pixels = {0, 0, 0, 255};
    viewer.showImage(image, L"C:\\outside\\sample.png"); registered(viewer.window());
    viewer.message(L"Select a file."); registered(viewer.window());
    // New split panes share the same handler but register their own body windows.
    split.create(parent, instance); split.setFileDropTarget(target);
    MoveWindow(split.window(), 320, 34, 320, 366, FALSE);
    registered(split.window()); registered(viewer.window());
    viewer.setFileDropTarget(nullptr);
    check(RegisterDragDrop(viewer.window(), target) == S_OK, "Detaching the editor body did not revoke its drop target");
    RevokeDragDrop(viewer.window());
    viewer.setFileDropTarget(target);
    const auto body = viewer.window(); DestroyWindow(body);
    registered(split.window()); // Destroying one body must retain the other registration.
    check(!IsWindow(body), "The editor body did not close");
}
void continuousDraggedTab() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto root = CreateWindowExW(0, L"STATIC", L"Tab drag test", WS_POPUP | WS_CLIPCHILDREN,
        100, 100, 480, 320, nullptr, nullptr, instance, nullptr);
    check(root != nullptr, "Cannot create tab drag owner");
    struct Owner { HWND window; ~Owner() { DestroyWindow(window); } } owner{root};
    const auto parent = CreateWindowExW(0, L"STATIC", L"Project", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        25, 30, 400, 240, root, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create tab drag project");
    ShowWindow(root, SW_SHOWNOACTIVATE);
    HWND tabWindow = nullptr;
    {
        DropPreview destination, tab;
        destination.create(parent, instance); tab.create(parent, instance, true);
        for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, L"PoppyDraggedTab", nullptr));)
            if (GetWindow(window, GW_OWNER) == root) { tabWindow = window; break; }
        const auto dropWindow = FindWindowExW(parent, nullptr, L"PoppyDropPreview", nullptr);
        check(tabWindow && dropWindow && !IsChild(parent, tabWindow), "The dragged tab is clipped to the project");
        struct Paints { unsigned count = 0; RECT bounds{}; } tabPaints, dropPaints;
        const auto observe = [](HWND window, UINT message, WPARAM a, LPARAM b, UINT_PTR, DWORD_PTR data) -> LRESULT {
            const auto result = DefSubclassProc(window, message, a, b);
            if (message == WM_PAINT) {
                auto& paints = *reinterpret_cast<Paints*>(data);
                ++paints.count; GetWindowRect(window, &paints.bounds);
            }
            return result;
        };
        check(SetWindowSubclass(tabWindow, observe, 1, reinterpret_cast<DWORD_PTR>(&tabPaints)) &&
              SetWindowSubclass(dropWindow, observe, 1, reinterpret_cast<DWORD_PTR>(&dropPaints)), "Cannot observe drag painting");
        const RECT body{0, 34, 400, 240};
        destination.show(body);
        const auto dropCount = dropPaints.count;
        for (int i = 0; i < 10; ++i) destination.show(body);
        check(dropPaints.count == dropCount, "Moving inside a destination repaints the entire preview");
        SetCapture(parent);
        const auto focus = GetFocus();
        // Keep the underlying view invalid and do not pump queued WM_PAINT.
        // Every drag step must still present at the new cursor position.
        for (int i = 0; i < 12; ++i) {
            const RECT rect{-40 + i * 45, -20 + i * 30, 120 + i * 45, 14 + i * 30};
            auto expected = rect;
            MapWindowPoints(parent, nullptr, reinterpret_cast<POINT*>(&expected), 2);
            InvalidateRect(parent, nullptr, FALSE);
            const auto before = tabPaints.count;
            tab.show(rect, L"Dragged tab 日本語", true);
            check(tabPaints.count > before && EqualRect(&tabPaints.bounds, &expected),
                "Continuous mouse input deferred the dragged tab paint or painted at the old position");
            check(!GetUpdateRect(tabWindow, nullptr, FALSE), "The dragged tab still has a pending paint");
            check(GetCapture() == parent && GetFocus() == focus, "The dragged tab stole capture or focus");
        }
        check(SendMessageW(tabWindow, WM_NCHITTEST, 0, 0) == HTTRANSPARENT &&
              SendMessageW(tabWindow, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "The dragged tab intercepts mouse input");
        ReleaseCapture(); tab.hide(); destination.hide();
        check(!IsWindowVisible(tabWindow) && !IsWindowVisible(dropWindow), "Cancelling left a drag preview visible");
        RemoveWindowSubclass(tabWindow, observe, 1); RemoveWindowSubclass(dropWindow, observe, 1);
    }
    check(!IsWindow(tabWindow), "Destroying a project left its dragged tab popup alive");
}
void terminalDropPreviewClipping() {
    const auto instance = GetModuleHandleW(nullptr);
    Terminal terminal;
    const auto parent = CreateWindowExW(0, L"STATIC", L"Terminal drop test", WS_POPUP | WS_CLIPCHILDREN,
        100, 100, 440, 300, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create terminal drop test owner");
    struct Owner { HWND window; ~Owner() { DestroyWindow(window); } } owner{parent};
    terminal.create(parent, instance);
    MoveWindow(terminal.window(), 20, 34, 400, 240, FALSE);
    DropPreview destination; destination.create(parent, instance);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    RECT client{}; GetClientRect(terminal.window(), &client);
    auto body = client;
    MapWindowPoints(terminal.window(), parent, reinterpret_cast<POINT*>(&body), 2);
    const LONG middleX = (body.left + body.right) / 2, middleY = (body.top + body.bottom) / 2;
    const RECT previews[] = {
        {body.left, body.top, body.right, middleY}, body,
        {body.left, body.top, middleX, body.bottom},
        {middleX, body.top, body.right, body.bottom},
        {body.left, middleY, body.right, body.bottom},
        {body.left, body.top, body.right, middleY}
    };
    const auto checkClipping = [&](RECT preview, bool visible) {
        MapWindowPoints(parent, terminal.window(), reinterpret_cast<POINT*>(&preview), 2);
        const auto dc = GetDC(terminal.window());
        check(dc != nullptr, "Cannot inspect the terminal's visible drawing region");
        struct Release { HWND window; HDC dc; ~Release() { ReleaseDC(window, dc); } } release{terminal.window(), dc};
        for (int y = 1; y < 8; y += 2) for (int x = 1; x < 8; x += 2) {
            const POINT point{client.right * x / 8, client.bottom * y / 8};
            const bool covered = visible && PtInRect(&preview, point);
            check((PtVisible(dc, point.x, point.y) != FALSE) == !covered,
                "The terminal can draw over a drop preview or clips an uncovered area");
        }
    };
    // Reproduce top -> center -> all edges, then repaint the terminal while
    // the destination stays still. Visibility and bounds alone miss this bug.
    for (const auto& preview : previews) {
        destination.show(preview);
        checkClipping(preview, true);
        RedrawWindow(terminal.window(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        checkClipping(preview, true);
        destination.show(preview);
        checkClipping(preview, true);
    }
    destination.hide();
    checkClipping(body, false);
}
void shutdownProgress() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Shutdown test", WS_POPUP, 100, 100, 1040, 720, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create shutdown test owner");
    struct Owner { HWND window; ~Owner() { DestroyWindow(window); } } owner{parent};
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    {
        ShutdownDialog dialog(parent, instance);
        check(!IsWindowEnabled(parent) && IsWindowVisible(dialog.window()), "Shutdown did not block the workspace behind the dialog");
        const auto shade = GetWindow(dialog.window(), GW_OWNER);
        BYTE alpha = 0; DWORD flags = 0;
        check(GetWindow(shade, GW_OWNER) == parent && IsWindowVisible(shade), "Shutdown shade is not owned by the workspace");
        check(GetLayeredWindowAttributes(shade, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha > 0 && alpha < 255, "Shutdown backdrop is not translucent");
        RECT parentRect{}, shadeRect{}; GetWindowRect(parent, &parentRect); GetWindowRect(shade, &shadeRect);
        check(EqualRect(&parentRect, &shadeRect), "Shutdown shade does not cover the whole owner");
        struct Observation { unsigned timers = 0, paints = 0, callbacks = 0; } observed;
        const auto observe = [](HWND window, UINT message, WPARAM a, LPARAM b, UINT_PTR, DWORD_PTR data) -> LRESULT {
            auto& observed = *reinterpret_cast<Observation*>(data);
            if (message == WM_TIMER) ++observed.timers;
            if (message == WM_PAINT) ++observed.paints;
            if (message == WM_APP+107) ++observed.callbacks;
            return DefSubclassProc(window, message, a, b);
        };
        check(SetWindowSubclass(dialog.window(), observe, 1, reinterpret_cast<DWORD_PTR>(&observed)) != FALSE, "Cannot observe shutdown animation");
        check(SetWindowSubclass(parent, observe, 1, reinterpret_cast<DWORD_PTR>(&observed)) != FALSE, "Cannot observe shutdown input blocking");
        for (const auto dpi : {96u, 144u, 192u}) {
            SendMessageW(dialog.window(), WM_DPICHANGED, MAKELONG(dpi, dpi), 0);
            RECT rect{}; GetWindowRect(dialog.window(), &rect);
            check(rect.right > rect.left && rect.bottom > rect.top, "Shutdown dialog disappeared after a DPI change");
        }
        dialog.stage(ShutdownDialog::Stage::Stopping, L"Project 1 / 2 · 日本語");
        dialog.detail(L"Stopping Docker containers and WSL processes · Shell");
        PostMessageW(parent, WM_APP+107, 0, 0);
        PostMessageW(dialog.window(), WM_CLOSE, 0, 0);
        Handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        check(static_cast<bool>(done), "Cannot create shutdown wait fixture");
        check(SetTimer(dialog.window(), 2, 240, [](HWND window, UINT, UINT_PTR id, DWORD) {
            SetEvent(reinterpret_cast<HANDLE>(GetPropW(window, L"ShutdownTestDone"))); KillTimer(window, id);
        }) != 0, "Cannot schedule shutdown wait completion");
        SetPropW(dialog.window(), L"ShutdownTestDone", done.value);
        const auto uiThread = GetCurrentThreadId();
        dialog.wait([&] {
            check(GetCurrentThreadId() != uiThread, "Blocking shutdown work ran on the UI thread");
            check(WaitForSingleObject(done.value, 3000) == WAIT_OBJECT_0, "Shutdown dialog stopped processing timers while waiting");
        });
        RemovePropW(dialog.window(), L"ShutdownTestDone");
        check(observed.timers >= 2 && observed.paints >= 1, "Shutdown progress did not animate while cleanup was blocked");
        check(observed.callbacks == 0 && IsWindow(dialog.window()), "Shutdown reentered application work or accepted a premature close");
        bool propagated = false;
        try { dialog.wait([] { throw std::runtime_error("cleanup failure"); }); } catch (const std::runtime_error&) { propagated = true; }
        check(propagated, "Shutdown wait lost the cleanup error");
        RemoveWindowSubclass(parent, observe, 1);
        RemoveWindowSubclass(dialog.window(), observe, 1);
    }
    check(IsWindowEnabled(parent), "Shutdown dialog did not restore its owner");
}
void persistentErrorPopup() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Error notification test", WS_POPUP | WS_CLIPCHILDREN,
        80, 80, 900, 600, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create error popup owner");
    struct Owner { HWND value; ~Owner() { DestroyWindow(value); } } owner{parent};
    ShowWindow(parent, SW_SHOWNOACTIVATE); SetFocus(parent);
    ErrorPopup popup;
    for (const UINT dpi : {96u, 144u, 192u}) {
        popup.show(parent, "terminal:missing-event-socket", L"AI通知エラー · Codex", L"接続先が未設定です。\n\nAIコンテナを起動し直してください。", dpi);
        const auto window = popup.window();
        check(popup.visible() && GetFocus() == parent && IsWindowEnabled(parent), "An error popup stole focus or blocked the workspace");
        const auto body = FindWindowExW(window, nullptr, L"PoppyViewer", nullptr);
        check(body != nullptr, "Error details are not selectable and scrollable");
        RECT bounds{}; GetWindowRect(window, &bounds);
        MONITORINFO monitor{sizeof(monitor), {}, {}, 0}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        check(bounds.left >= monitor.rcWork.left && bounds.top >= monitor.rcWork.top && bounds.right <= monitor.rcWork.right && bounds.bottom <= monitor.rcWork.bottom,
            "The error popup escaped the monitor at a different DPI");
        SendMessageW(window, WM_ACTIVATEAPP, FALSE, 0);
        SendMessageW(window, WM_MOUSELEAVE, 0, 0);
        SendMessageW(window, WM_TIMER, 1, 0);
        SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        check(popup.visible(), "An error disappeared without pressing its close button");
        popup.show(parent, "terminal:missing-event-socket", L"duplicate", L"duplicate", dpi);
        wchar_t title[128]{}; GetWindowTextW(window, title, 128);
        check(std::wstring(title).find(L"(1)") != std::wstring::npos, "A repeated failure duplicated a persistent error");
        popup.show(parent, "terminal:relay-disconnected", L"接続切断", L"別のエラーも保持します。", dpi);
        GetWindowTextW(window, title, 128);
        check(std::wstring(title).find(L"(2)") != std::wstring::npos, "A second error replaced an unacknowledged error");
        SendMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
        check(!popup.visible(), "The error popup close button did not dismiss it");
    }
    // Exercise the actual ConPTY path as well as the model's OSC parser.
    Terminal terminal;
    terminal.create(parent, instance); terminal.setDirectory(fs::temp_directory_path());
    MoveWindow(terminal.window(), 0, 0, 600, 120, FALSE);
    terminal.notificationError = [&](const std::string& code) {
        check(code == "missing-event-socket", "ConPTY changed the notification error code");
        popup.show(parent, "conpty:missing-event-socket", L"AI通知エラー", L"通知ソケットが未設定です。", 96);
    };
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    terminal.setCommand({std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe", L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-Command",
        L"[Console]::Write([char]27 + ']777;poppy;error;missing-event-socket' + [char]7)"});
    check(terminal.launch(), "Could not start notification diagnostic fixture");
    const auto start = GetTickCount64();
    while (!popup.visible() && GetTickCount64() - start < 5000) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Terminal::pump(); Sleep(5);
    }
    check(popup.visible(), "A missing notification socket did not reach the popup through ConPTY");
    terminal.stop();
}
void statusBarLimits() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize status tooltip controls");
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Status bar test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1400, 200, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create status bar test parent");
    StatusBar bar; bar.create(parent, instance); bar.message(L"Ready"); bar.editor(L"UTF-8  ·  CRLF");
    FileIndex index; index.files.resize(1234); index.storageBytes = 1024 * 1024;
    const auto watch = watchLimitStatus(WatchUsage{1234, 262144, 524288, false, {}});
    bar.update(L"main", {watch, fileLimitStatus(&index, false), treeLimitStatus(1234, 50000, 10000)});
    HWND tooltip = nullptr;
    struct Context { HWND owner; HWND* tooltip; } context{bar.window(), &tooltip};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        const auto& context = *reinterpret_cast<Context*>(data);
        wchar_t name[64]{}; GetClassNameW(window, name, 64);
        if (GetWindow(window, GW_OWNER) == context.owner && std::wstring(name) == TOOLTIPS_CLASSW) { *context.tooltip = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    check(tooltip != nullptr, "Status bar hover explanations are missing");
    for (const auto dpi : {96u, 144u, 192u}) {
        bar.setDpi(dpi);
        for (const int width : {1400, 720, 420}) {
            const int scaledWidth = MulDiv(width, dpi, 96), height = MulDiv(26, dpi, 96);
            SetWindowPos(bar.window(), nullptr, 0, 0, scaledWidth, height, SWP_NOZORDER | SWP_NOACTIVATE);
            const auto count = SendMessageW(tooltip, TTM_GETTOOLCOUNT, 0, 0);
            bool hasWatch = false, hasOverflow = false, hasEditor = false; std::vector<RECT> bounds;
            for (LRESULT i = 0; i < count; ++i) {
                TOOLINFOW tool{}; tool.cbSize = sizeof(tool);
                check(SendMessageW(tooltip, TTM_ENUMTOOLSW, i, reinterpret_cast<LPARAM>(&tool)) != FALSE, "Cannot read status bar hover bounds");
                check(tool.rect.left >= 0 && tool.rect.right <= scaledWidth && tool.rect.top == 0 && tool.rect.bottom == height && tool.rect.left <= tool.rect.right, "Status bar tools escaped their layout after resizing");
                for (const auto& other : bounds) { RECT overlap{}; check(!IntersectRect(&overlap, &other, &tool.rect), "Status bar messages and limit counters overlap"); }
                bounds.push_back(tool.rect);
                NMTTDISPINFOW display{}; display.hdr = {tooltip, tool.uId, TTN_GETDISPINFOW};
                SendMessageW(bar.window(), WM_NOTIFY, tool.uId, reinterpret_cast<LPARAM>(&display));
                const std::wstring tip = display.lpszText ? display.lpszText : L"";
                hasWatch |= tip.find(L"WSL directory watches") != tip.npos;
                hasOverflow |= tip.find(L"Files 1,234/400,000") != tip.npos && tip.find(L"WSL file list") != tip.npos;
                hasEditor |= tip == L"UTF-8  ·  CRLF";
            }
            check(hasWatch, "Narrowing the status bar lost watch-limit explanations");
            check(hasEditor, "Resizing or changing DPI lost the editor format status");
            if (width == 420) check(hasOverflow, "Hidden limit counters were not exposed in the Limits tooltip");
        }
    }
    const auto withEditor = SendMessageW(tooltip, TTM_GETTOOLCOUNT, 0, 0);
    bar.editor(L"");
    check(SendMessageW(tooltip, TTM_GETTOOLCOUNT, 0, 0) == withEditor - 1, "Clearing editor status retained its tooltip");
    DestroyWindow(parent);
}
// WIC provides inspectable Direct2D pixels without a DC or GDI bitmap.
struct Bitmap {
    static constexpr int width = 512, height = 128;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    std::vector<BYTE> pixels = std::vector<BYTE>(width * height * 4);
    Bitmap() {
        check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory), reinterpret_cast<void**>(factory.put()))), "Could not create the WIC factory");
        check(SUCCEEDED(factory->CreateBitmap(width, height, GUID_WICPixelFormat32bppBGR, WICBitmapCacheOnLoad, bitmap.put())), "Could not create the WIC bitmap");
    }
    void read() { check(SUCCEEDED(bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())), "Could not read WIC pixels"); }
    RECT bounds() const { return {0, 0, width, height}; }
    COLORREF pixel(int x, int y) const {
        const auto offset = static_cast<size_t>((y * width + x) * 4);
        return RGB(pixels[offset + 2], pixels[offset + 1], pixels[offset]);
    }
};
void finish(Renderer& renderer) { check(SUCCEEDED(renderer.end()), "Direct2D drawing failed"); }
void treeMultipleSelection() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Multiple selection test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 640, 320, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the multiple selection test window");
    Tree tree;
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    tree.create(parent, instance, L"EXPLORER", 1); tree.multipleSelection = true;
    SetWindowPos(tree.window(), nullptr, 0, 0, 640, 320, SWP_NOZORDER | SWP_SHOWWINDOW); ShowWindow(parent, SW_SHOWNOACTIVATE);
    const auto root = tree.insert(0, L"project", true, 0), target = tree.insert(root, L"target", true, 0);
    const auto a = tree.insert(root, L"a.txt", false, 0), b = tree.insert(root, L"b.txt", false, 0), c = tree.insert(root, L"c.txt", false, 0);
    tree.expand(root, true);
    std::vector<TreeItem> deleted, copied, moved; bool cut = false; TreeItem renamed = 0; unsigned activations = 0;
    tree.activated = [&](TreeItem) { ++activations; };
    tree.canDelete = tree.canCopy = tree.canDrag = [&](TreeItem id) { return id != root; };
    tree.deleteRequested = [&](const auto& ids) { deleted = ids; };
    tree.copyRequested = [&](const auto& ids, bool value) { copied = ids; cut = value; };
    tree.renameRequested = [&](TreeItem id) { renamed = id; };
    tree.canDrop = [&](TreeItem id, TreeItem parent) { return parent == target && id != target; };
    tree.moveCommitted = [&](const auto& ids, TreeItem parent) { check(parent == target, "Multiple drag chose another folder"); moved = ids; };
    const auto point = [&](TreeItem id) { return POINT{100, static_cast<LONG>(SendMessageW(tree.window(), treeGetRow, id, 0) * SendMessageW(tree.window(), treeGetRowHeight, 0, 0) + 5)}; };
    const auto clickRow = [&](TreeItem id, WPARAM modifiers) {
        const auto p = point(id);
        SendMessageW(tree.window(), WM_LBUTTONDOWN, MK_LBUTTON | modifiers, MAKELPARAM(p.x, p.y));
        SendMessageW(tree.window(), WM_LBUTTONUP, modifiers, MAKELPARAM(p.x, p.y));
    };
    const auto key = [&](WPARAM code) {
        BYTE previous[256]{}, keys[256]{}; GetKeyboardState(previous); keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        SetKeyboardState(keys); SendMessageW(tree.window(), WM_KEYDOWN, code, 0); SetKeyboardState(previous);
    };
    clickRow(a, 0); check(activations == 1, "A normal file click did not activate it");
    clickRow(c, MK_CONTROL);
    check(tree.selections() == std::vector<TreeItem>({a, c}) && activations == 1, "Ctrl click activated a file or lost existing selection");
    key('C'); check(copied == std::vector<TreeItem>({a, c}) && !cut, "Copy did not receive the whole selection");
    key('X'); check(copied == std::vector<TreeItem>({a, c}) && cut, "Cut did not receive the whole selection");
    SendMessageW(tree.window(), WM_KEYDOWN, VK_DELETE, 0);
    check(deleted == std::vector<TreeItem>({a, c}), "Delete did not receive the whole selection");
    auto menuPoint = point(c); ClientToScreen(tree.window(), &menuPoint);
    check(SetTimer(tree.window(), 99, 10, [](HWND window, UINT, UINT_PTR id, DWORD) { EndMenu(); KillTimer(window, id); }) != 0, "Cannot schedule menu cancellation");
    SendMessageW(tree.window(), WM_CONTEXTMENU, reinterpret_cast<WPARAM>(tree.window()), MAKELPARAM(menuPoint.x, menuPoint.y));
    KillTimer(tree.window(), 99);
    check(tree.selections() == std::vector<TreeItem>({a, c}), "Right click collapsed multiple selection");
    const auto origin = point(a), destination = point(target);
    SendMessageW(tree.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(origin.x, origin.y));
    SendMessageW(tree.window(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(destination.x, destination.y));
    SendMessageW(tree.window(), WM_LBUTTONUP, 0, MAKELPARAM(destination.x, destination.y));
    check(moved == std::vector<TreeItem>({a, c}), "Dragging a selected row did not move the whole selection");
    clickRow(a, 0); clickRow(c, MK_SHIFT);
    check(tree.selections() == std::vector<TreeItem>({a, b, c}), "Shift click did not select the visible range");
    clickRow(c, MK_CONTROL); clickRow(b, MK_CONTROL);
    check(tree.selections() == std::vector<TreeItem>({a}), "Ctrl clicks did not remove rows");
    SendMessageW(tree.window(), WM_KEYDOWN, VK_F2, 0);
    check(renamed == a, "Rename targeted a deselected focused row");
    key('A'); check(tree.selections().size() == 4 && !SendMessageW(tree.window(), treeIsSelected, root, 0), "Select all included the project root");
    SendMessageW(tree.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 300));
    SendMessageW(tree.window(), WM_LBUTTONUP, 0, MAKELPARAM(100, 300));
    check(tree.selections().empty(), "Clicking empty space did not clear multiple selection");
}
void commitMessagePopup() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Commit hover test", WS_POPUP | WS_CLIPCHILDREN,
        20, 20, 900, 650, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the commit hover test window");
    Tree tree;
    struct Owner { HWND value; ~Owner() { DestroyWindow(value); } } owner{parent};
    POINT originalCursor{}; GetCursorPos(&originalCursor);
    struct Cursor { POINT value; ~Cursor() { SetCursorPos(value.x, value.y); } } cursor{originalCursor};
    tree.create(parent, instance, L"SOURCE CONTROL", 1);
    const auto branch = tree.insert(0, L"main", true, 0);
    const auto commit = tree.insert(branch, L"abc123  subject", true, 0);
    tree.expand(branch, true); tree.select(branch, false);
    const std::wstring message = L"日本語の件名 🌸\n\n  body with tabs\tand symbols ` $() %\n\n" + std::wstring(maxLineColumns + 100, L'x') + L"🌸\nend\n";
    unsigned activations = 0;
    tree.activated = [&](TreeItem) { ++activations; };
    tree.hoverMessage = [&](TreeItem id) { return id == commit ? message : std::wstring{}; };
    tree.hoverActionsOnly = true;
    tree.actionLabel = [&](TreeItem id) { return id == commit ? L"Open All Changes" : L""; };
    const auto popup = [] {
        HWND result = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
            wchar_t name[64]{}; GetClassNameW(window, name, 64);
            if (std::wstring(name) == L"PoppyMessagePopup") { *reinterpret_cast<HWND*>(data) = window; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        return result;
    };
    ShowWindow(parent, SW_SHOW);
    for (const auto dpi : {96u, 144u, 192u}) {
        tree.setDpi(dpi);
        const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        SetWindowPos(tree.window(), nullptr, 0, 0, px(245), px(240), SWP_NOZORDER | SWP_SHOWWINDOW);
        SetFocus(tree.window());
        const int y = static_cast<int>(SendMessageW(tree.window(), treeGetRow, commit, 0) * SendMessageW(tree.window(), treeGetRowHeight, 0, 0) + px(5));
        const auto hover = [&](int x, int rowY) {
            POINT point{x, rowY}; ClientToScreen(tree.window(), &point); SetCursorPos(point.x, point.y);
            SendMessageW(tree.window(), WM_MOUSEMOVE, 0, MAKELPARAM(x, rowY));
        };
        hover(px(60), y);
        check(!IsWindowVisible(popup()), "Commit popup opened before the hover delay");
        SendMessageW(tree.window(), WM_TIMER, 3, 0);
        const auto tip = popup();
        check(tip && IsWindowVisible(tip) && GetFocus() == tree.window(), "Hover failed to show full commit text without taking focus");
        check(tree.selection() == branch && !activations, "Hover changed the selected commit or opened its files");
        RECT bounds{}; GetWindowRect(tip, &bounds);
        MONITORINFO monitor{sizeof(monitor), {}, {}, 0}; GetMonitorInfoW(MonitorFromWindow(tip, MONITOR_DEFAULTTONEAREST), &monitor);
        check(bounds.left >= monitor.rcWork.left && bounds.top >= monitor.rcWork.top && bounds.right <= monitor.rcWork.right && bounds.bottom <= monitor.rcWork.bottom,
            "Commit popup escaped the monitor work area");
        const auto body = FindWindowExW(tip, nullptr, L"PoppyViewer", nullptr);
        check(body != nullptr, "Commit popup has no selectable text body");
        POINT inside{px(40), px(15)}; ClientToScreen(body, &inside); SetCursorPos(inside.x, inside.y);
        SendMessageW(tree.window(), WM_MOUSELEAVE, 0, 0);
        SendMessageW(tip, WM_TIMER, 1, 0); Sleep(320); SendMessageW(tip, WM_TIMER, 1, 0);
        check(IsWindowVisible(tip), "Moving into the popup dismissed the commit message");
        SendMessageW(body, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px(12), px(5)));
        SendMessageW(body, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(px(60), px(5)));
        SendMessageW(body, WM_LBUTTONUP, 0, MAKELPARAM(px(60), px(5)));
        BYTE previous[256]{}, keys[256]{}; GetKeyboardState(previous); keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
        SetKeyboardState(keys); SendMessageW(body, WM_KEYDOWN, 'C', 0); SetKeyboardState(previous);
        const auto selected = clipboardText(body);
        check(!selected.empty() && selected != message && message.find(selected) != message.npos, "Commit text could not be selected and copied");
        SetKeyboardState(keys); SendMessageW(body, WM_KEYDOWN, 'A', 0); SendMessageW(body, WM_KEYDOWN, 'C', 0); SetKeyboardState(previous);
        check(clipboardText(body) == message, "Select all / copy lost full commit text or whitespace");
        SendMessageW(body, WM_KEYDOWN, VK_END, 0);
        check(GetScrollPos(body, SB_VERT) > 0, "Long commit text could not be scrolled");
        check(copyText(tip, L"sentinel"), "Cannot prepare the full-message copy test");
        RECT client{}; GetClientRect(tip, &client);
        const auto copyPoint = MAKELPARAM(client.right - px(50), px(16));
        SendMessageW(tip, WM_LBUTTONDOWN, MK_LBUTTON, copyPoint); SendMessageW(tip, WM_LBUTTONUP, 0, copyPoint);
        check(clipboardText(tip) == message && IsWindowVisible(tip), "Copy all changed the message or dismissed the popup");
        SendMessageW(body, WM_KEYDOWN, VK_ESCAPE, 0);
        check(!IsWindowVisible(tip) && GetFocus() == tree.window(), "Escape did not close the commit popup and restore focus");
        hover(px(60), y); SendMessageW(tree.window(), WM_TIMER, 3, 0);
        check(IsWindowVisible(tip), "Returning to the same commit after Escape did not reopen its message");
        if (dpi == 96) {
            POINT outside{5, 5}; ClientToScreen(tree.window(), &outside); SetCursorPos(outside.x, outside.y);
            SendMessageW(tip, WM_TIMER, 1, 0); Sleep(320); SendMessageW(tip, WM_TIMER, 1, 0);
            check(!IsWindowVisible(tip), "Leaving both hover regions did not dismiss the commit message");
        }
        hover(px(60), 5); SendMessageW(tree.window(), WM_TIMER, 3, 0);
        check(!IsWindowVisible(tip), "A branch row showed a commit message");
        hover(px(230), y); SendMessageW(tree.window(), WM_TIMER, 3, 0);
        check(!IsWindowVisible(tip), "Commit text obscured the existing action tooltip");
        hover(px(60), y); SendMessageW(tree.window(), WM_TIMER, 3, 0);
        check(IsWindowVisible(tip), "Returning to a commit did not reopen its message");
        tree.restoreScroll(0, 1);
        check(!IsWindowVisible(tip), "Scrolling the tree retained an obsolete commit popup");
    }
    ShowWindow(tree.window(), SW_HIDE);
    SendMessageW(tree.window(), WM_TIMER, 3, 0);
    check(!IsWindowVisible(popup()), "A hidden source-control pane opened a pending popup");
    ShowWindow(tree.window(), SW_SHOW);
    const int commitY = static_cast<int>(SendMessageW(tree.window(), treeGetRow, commit, 0) * SendMessageW(tree.window(), treeGetRowHeight, 0, 0) + 5);
    SendMessageW(tree.window(), WM_MOUSEMOVE, 0, MAKELPARAM(60, commitY));
    SendMessageW(tree.window(), WM_TIMER, 3, 0);
    check(IsWindowVisible(popup()), "Cannot reopen the commit popup before a history update");
    tree.erase(commit);
    check(!IsWindowVisible(popup()), "Removing a commit retained its popup");
}
void inlineTreeInput() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    const auto instance = GetModuleHandleW(nullptr);
    Tree tree;
    const auto parent = CreateWindowExW(0, L"STATIC", L"Inline rename test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 640, 320, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the inline rename test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    tree.create(parent, instance, L"EXPLORER", 1);
    SetWindowPos(tree.window(), nullptr, 0, 0, 640, 320, SWP_NOZORDER | SWP_SHOWWINDOW); ShowWindow(parent, SW_SHOWNOACTIVATE);
    const auto root = tree.insert(0, L"project", true, 0);
    const auto file = tree.insert(root, L"sample.txt", false, 0, accent); tree.expand(root, true);
    std::wstring committed; tree.renameCommitted = [&](TreeItem id, const std::wstring& text) { check(id == file, "Rename committed another row"); committed = text; };
    const auto editor = [&] { return FindWindowExW(tree.window(), nullptr, treeInputClassName, nullptr); };
    const auto text = [](HWND window) { std::wstring value(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0'); value.resize(GetWindowTextW(window, value.data(), static_cast<int>(value.size()))); return value; };
    for (const auto dpi : {96u, 144u, 192u}) {
        tree.setDpi(dpi); tree.rename(file, L"sample.txt"); const auto input = editor();
        check(input && !FindWindowExW(tree.window(), nullptr, L"EDIT", nullptr), "Rename still uses a standard Windows input");
        check(!(GetWindowLongPtrW(input, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) && !(GetWindowLongPtrW(input, GWL_STYLE) & WS_BORDER), "Rename has an input frame");
        RECT bounds{}; GetWindowRect(input, &bounds); MapWindowPoints(nullptr, tree.window(), reinterpret_cast<POINT*>(&bounds), 2);
        const auto height = SendMessageW(tree.window(), treeGetRowHeight, 0, 0);
        check(bounds.left == 2 * MulDiv(18, dpi, 96) + MulDiv(5, dpi, 96) && bounds.top == height && bounds.bottom == 2 * height, "Rename moved the filename or its baseline");
        check(SendMessageW(input, EM_GETSEL, 0, 0) == MAKELRESULT(0, 6), "Rename did not select only the filename stem");
        SendMessageW(input, WM_CHAR, L'名', 0); check(text(input) == L"名.txt", "Typing did not replace the selected stem");
        SendMessageW(input, WM_UNDO, 0, 0); check(text(input) == L"sample.txt", "Undo did not restore the filename");
        SendMessageW(input, EM_SETSEL, 0, 6);
        SendMessageW(input, WM_CHAR, 0xd83d, 0); SendMessageW(input, WM_CHAR, 0xde42, 0);
        check(text(input) == L"\U0001f642.txt", "Rename split a surrogate pair");
        SendMessageW(input, WM_KEYDOWN, VK_BACK, 0); check(text(input) == L".txt", "Backspace split a surrogate pair");
        check(copyText(input, L"変更 名"), "Cannot seed the rename clipboard");
        SendMessageW(input, WM_PASTE, 0, 0); check(text(input) == L"変更 名.txt", "Rename did not paste clipboard text");
        SendMessageW(input, WM_IME_STARTCOMPOSITION, 0, 0); SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0);
        check(editor() == input && committed.empty(), "IME Enter committed a rename before composition ended");
        SendMessageW(input, WM_IME_ENDCOMPOSITION, 0, 0);
        tree.setDpi(dpi == 192 ? 96 : 192);
        check(editor() == input && text(input) == L"変更 名.txt", "DPI change lost the rename input");
        SetWindowPos(tree.window(), nullptr, 0, 0, 400, 320, SWP_NOZORDER);
        GetWindowRect(input, &bounds); MapWindowPoints(nullptr, tree.window(), reinterpret_cast<POINT*>(&bounds), 2);
        check(bounds.right <= 400 && bounds.right > bounds.left, "Rename did not follow the sidebar resize");
        SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0);
        check(!editor() && committed == L"変更 名.txt", "Enter did not commit the inline name"); committed.clear();
        tree.rename(file, L"sample.txt"); SendMessageW(editor(), WM_KEYDOWN, VK_ESCAPE, 0);
        check(!editor() && committed.empty(), "Escape committed the inline name");
        tree.rename(file, L"sample.txt"); SetFocus(parent);
        check(!editor() && committed.empty(), "Losing focus did not cancel the inline name");
        tree.rename(file, L"sample.txt"); tree.expand(root, false);
        check(!editor() && committed.empty(), "Collapsing a folder left its hidden rename active"); tree.expand(root, true);
        SetWindowPos(tree.window(), nullptr, 0, 0, 640, 320, SWP_NOZORDER);
    }
    // Pixel-for-pixel comparison verifies font, baseline, text color, clipping,
    // and the unframed row background at each supported display scale.
    for (const auto dpi : {96u, 144u, 192u}) {
        TreeInput input; input.setDpi(dpi);
        const std::wstring name = L"日本語 sample \U0001f642.txt";
        const auto window = input.create(parent, instance, name, accent);
        SetFocus(parent);
        Renderer actual, expected; actual.setDpi(dpi); expected.setDpi(dpi);
        Bitmap pixels, reference;
        const RECT row{0, 0, Bitmap::width, MulDiv(26, dpi, 96)};
        check(actual.begin(pixels.bitmap.get()), "Cannot render the inline name"); actual.fill(pixels.bounds(), panel); input.draw(actual, row); finish(actual); pixels.read();
        check(expected.begin(reference.bitmap.get()), "Cannot render the reference tree label"); expected.fill(reference.bounds(), panel); expected.fill(row, RGB(50, 70, 94)); expected.label(row, name, accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE); finish(expected); reference.read();
        check(pixels.pixels == reference.pixels, "Inline rename changed the tree font or row background");
        SetWindowPos(window, nullptr, 0, 0, row.right, row.bottom, SWP_NOZORDER);
        const auto longName = std::wstring(220, L'w') + L".txt";
        SetWindowTextW(window, longName.c_str()); SendMessageW(window, EM_SETSEL, 0, 220); SendMessageW(window, WM_CHAR, L'名', 0);
        check(input.text() == L"名.txt", "Replacing a long selected filename lost its extension");
        check(actual.begin(pixels.bitmap.get()), "Cannot render the shortened filename"); actual.fill(pixels.bounds(), panel); input.draw(actual, row); finish(actual); pixels.read();
        check(expected.begin(reference.bitmap.get()), "Cannot render the shortened reference"); expected.fill(reference.bounds(), panel); expected.fill(row, RGB(50, 70, 94)); expected.label(row, L"名.txt", accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE); finish(expected); reference.read();
        check(pixels.pixels == reference.pixels, "Shortening the filename left its first characters scrolled out of view");
        DestroyWindow(window);
    }
    TreeInput terminalName; terminalName.setDpi(96); terminalName.commitOnFocusLoss = true;
    const auto input = terminalName.create(parent, instance, L"Shell", foreground);
    MoveWindow(input, 0, 0, 200, 32, FALSE);
    int completions = 0; bool commit = false;
    terminalName.finished = [&](bool value) { ++completions; commit = value; };
    SetFocus(input);
    SendMessageW(input, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0); SendMessageW(input, WM_KEYDOWN, VK_ESCAPE, 0);
    check(completions == 0, "IME Enter or Escape finished a terminal name");
    SendMessageW(input, WM_IME_ENDCOMPOSITION, 0, 0);
    SendMessageW(input, WM_KEYDOWN, VK_ESCAPE, 0);
    check(completions == 1 && !commit, "Escape did not cancel a terminal name");
    SetFocus(parent);
    check(completions == 2 && commit, "Losing focus did not finish a terminal name");
    DestroyWindow(input);

}
void inlineTreeCreation() {
    const auto instance = GetModuleHandleW(nullptr);
    Tree tree;
    const auto parent = CreateWindowExW(0, L"STATIC", L"Inline creation test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 640, 480, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the inline creation test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    tree.create(parent, instance, L"EXPLORER", 1);
    SetWindowPos(tree.window(), nullptr, 0, 0, 640, 480, SWP_NOZORDER | SWP_SHOWWINDOW); ShowWindow(parent, SW_SHOWNOACTIVATE);
    const auto root = tree.insert(0, L"project", true, 0);
    const auto folder = tree.insert(root, L"folder", true, 0);
    const auto sibling = tree.insert(root, L"sibling.txt", false, 0);
    tree.expand(root, true);
    tree.canCreate = [&](TreeItem id) { return id == root || id == folder; };
    TreeItem activated = 0; tree.activated = [&](TreeItem id) { activated = id; };
    size_t commits = 0; TreeItem destination = 0; bool directory = false; std::wstring name;
    tree.createCommitted = [&](TreeItem id, const std::wstring& text, bool value) { ++commits; destination = id; directory = value; name = text; };
    const auto editor = [&] { return FindWindowExW(tree.window(), nullptr, treeInputClassName, nullptr); };
    const auto row = [&](TreeItem id) { return SendMessageW(tree.window(), treeGetRow, id, 0); };
    const auto bounds = [&] { RECT value{}; GetWindowRect(editor(), &value); MapWindowPoints(nullptr, tree.window(), reinterpret_cast<POINT*>(&value), 2); return value; };
    const auto checkPlacement = [&](UINT dpi, TreeItem target, int depth) {
        check(editor() != nullptr, "Creation did not show an input");
        const auto value = bounds();
        const auto height = SendMessageW(tree.window(), treeGetRowHeight, 0, 0);
        check(value.top == (row(target) + 1 - tree.top()) * height && value.bottom == value.top + height,
              "Creation input did not occupy the row below its anchor");
        check(value.left == (depth + 1) * MulDiv(18, dpi, 96) + MulDiv(5, dpi, 96), "Creation input did not use child indentation");
    };
    for (const auto dpi : {96u, 144u, 192u}) {
        tree.setDpi(dpi); tree.expand(folder, false);
        const auto originalCount = SendMessageW(tree.window(), treeGetCount, 0, 0);
        tree.createItem(folder, false); checkPlacement(dpi, folder, 2);
        check(tree.item(folder)->text == L"folder" && tree.item(folder)->expanded, "Creation changed or hid the parent folder label");
        check(row(sibling) == row(folder) + 2 && SendMessageW(tree.window(), treeGetCount, 0, 0) == originalCount,
              "Creation input overlapped a sibling or became a resource node");
        SetWindowTextW(editor(), L"path/新規.txt");
        // Simulate the asynchronous directory listing arriving while typing.
        const auto child = tree.insert(folder, L"existing.txt", false, 1);
        tree.sort(folder, [](uintptr_t a, uintptr_t b) { return a < b; });
        checkPlacement(dpi, folder, 2);
        check(row(child) == row(folder) + 2 && row(sibling) == row(folder) + 3, "Listing results overlapped the creation input");
        const auto resizedDpi = dpi == 192 ? 96u : 192u;
        tree.setDpi(resizedDpi); checkPlacement(resizedDpi, folder, 2);
        SetWindowPos(tree.window(), nullptr, 0, 0, 400, 480, SWP_NOZORDER);
        check(editor() && bounds().right <= 400, "Resizing lost or clipped the creation input");
        SendMessageW(editor(), WM_KEYDOWN, VK_RETURN, 0);
        check(!editor() && destination == folder && !directory && name == L"path/新規.txt", "Creation committed the wrong parent, name, or kind");
        check(row(child) == row(folder) + 1 && row(sibling) == row(folder) + 2, "Committing left an extra creation row");
        const auto committed = commits;
        tree.createItem(folder, true); SetWindowTextW(editor(), L"cancelled"); SendMessageW(editor(), WM_KEYDOWN, VK_ESCAPE, 0);
        check(!editor() && commits == committed && row(child) == row(folder) + 1, "Escape committed or retained the creation row");
        tree.createItem(folder, false);
        activated = 0;
        const auto clickHeight = SendMessageW(tree.window(), treeGetRowHeight, 0, 0);
        const auto y = (row(sibling) - tree.top()) * clickHeight + clickHeight / 2;
        SendMessageW(tree.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, y));
        SendMessageW(tree.window(), WM_LBUTTONUP, 0, MAKELPARAM(100, y));
        check(!editor() && commits == committed && activated == sibling, "Cancelling creation by clicking selected the wrong row");
        tree.createItem(folder, false); SetFocus(parent);
        check(!editor() && commits == committed, "Focus loss did not cancel creation");
        tree.createItem(folder, false); tree.expand(folder, false);
        check(!editor() && commits == committed && row(sibling) == row(folder) + 1, "Collapsing the parent retained the creation row");
        tree.createItem(root, true); checkPlacement(resizedDpi, root, 1);
        check(row(folder) == 2, "Root creation input overlapped an existing folder");
        SetWindowTextW(editor(), L"新規フォルダー"); SendMessageW(editor(), WM_KEYDOWN, VK_RETURN, 0);
        check(destination == root && directory && name == L"新規フォルダー" && row(folder) == 1, "New Folder committed the wrong destination or retained its input row");
        tree.createItem(folder, false); tree.clearChildren(folder);
        check(editor() != nullptr, "Refreshing children cancelled their parent's creation input");
        SendMessageW(editor(), WM_KEYDOWN, VK_ESCAPE, 0);
        SetWindowPos(tree.window(), nullptr, 0, 0, 640, 480, SWP_NOZORDER);
    }
    // A file near the end of a large directory must keep the input beside it.
    std::vector<TreeItem> files;
    {
        Tree::Batch batch(tree);
        for (uintptr_t i = 0; i < 120; ++i) files.push_back(tree.insert(folder, L"file" + std::to_wstring(i) + L".txt", false, i));
    }
    for (const auto dpi : {96u, 144u, 192u}) {
        tree.setDpi(dpi);
        for (const auto anchor : {files.front(), files[60], files.back(), sibling}) {
            const auto parent = tree.item(anchor)->parent;
            const auto label = tree.item(anchor)->text;
            const auto following = tree.next(anchor);
            const auto count = SendMessageW(tree.window(), treeGetCount, 0, 0);
            tree.select(anchor, false); activated = 0;
            tree.createItem(parent, false, anchor);
            checkPlacement(dpi, anchor, parent == folder ? 2 : 1);
            check(tree.selection() == anchor && !activated && tree.item(anchor)->text == label, "Creation changed or activated its selected anchor");
            if (following) check(row(following) == row(anchor) + 2, "Creation input overlapped the following sibling");
            check(SendMessageW(tree.window(), treeGetCount, 0, 0) == count, "Anchored creation became a resource node");
            SetWindowTextW(editor(), L"隣のファイル.txt");
            tree.sort(parent, [](uintptr_t a, uintptr_t b) { return a < b; });
            checkPlacement(dpi, anchor, parent == folder ? 2 : 1);
            SendMessageW(editor(), WM_KEYDOWN, VK_RETURN, 0);
            check(!editor() && destination == parent && !directory && name == L"隣のファイル.txt", "Anchored creation used the anchor file as its destination");
            if (following) check(row(following) == row(anchor) + 1, "Anchored creation left a row after committing");
            tree.createItem(parent, true, anchor);
            checkPlacement(dpi, anchor, parent == folder ? 2 : 1);
            SetWindowTextW(editor(), L"隣のフォルダー"); SendMessageW(editor(), WM_KEYDOWN, VK_RETURN, 0);
            check(destination == parent && directory && name == L"隣のフォルダー", "Folder creation beside a file used the wrong destination or kind");
        }
    }
    const auto committed = commits;
    tree.createItem(folder, false, files[60]); tree.erase(files[60]);
    check(!editor() && commits == committed, "Removing the anchor left its input active or committed it");
    tree.createItem(folder, false, files.front()); tree.expand(folder, false);
    check(!editor() && commits == committed, "Collapsing an anchored input's parent retained or committed the input");
    tree.createItem(folder, false, files.front()); tree.clearChildren(folder);
    check(!editor() && commits == committed, "Refreshing away the anchor left its input active or committed it");
    tree.setDpi(96); tree.restoreScroll(0, 0);
    const auto height = SendMessageW(tree.window(), treeGetRowHeight, 0, 0);
    SetWindowPos(tree.window(), nullptr, 0, 0, 640, 2 * static_cast<int>(height), SWP_NOZORDER);
    tree.createItem(folder, false); checkPlacement(96, folder, 2);
    check(tree.top() > 0 && bounds().bottom <= 2 * height, "Creation at the bottom did not scroll its input into view");
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE, 0, 0, 0, 0, 0}; GetScrollInfo(tree.window(), SB_VERT, &scroll);
    check(scroll.nMax == SendMessageW(tree.window(), treeGetCount, 0, 0), "Creation row was missing from the scrollbar range");
    SendMessageW(editor(), WM_KEYDOWN, VK_ESCAPE, 0);
    GetScrollInfo(tree.window(), SB_VERT, &scroll);
    check(scroll.nMax + 1 == SendMessageW(tree.window(), treeGetCount, 0, 0), "Cancellation left the creation row in the scrollbar range");
    tree.createItem(folder, false); tree.erase(folder);
    check(!editor(), "Removing the parent left its creation input active");
}
void gitCommitInput() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Commit input test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 700, 400, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create commit input test parent");
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    const auto first = ResourceId::wsl("Ubuntu", "/projects/first"), second = ResourceId::wsl("Ubuntu", "/projects/second");
    for (const auto dpi : {96u, 144u, 192u}) {
        GitCommitPane pane; pane.create(parent, instance); pane.setDpi(dpi);
        const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        SetWindowPos(pane.window(), nullptr, 0, 0, px(240), px(90), SWP_NOZORDER | SWP_SHOWWINDOW);
        const auto input = GetDlgItem(pane.window(), 1002);
        check(input != nullptr && !IsWindowEnabled(input), "Commit input was enabled without a repository");
        size_t requests = 0; ResourceId requestedRoot; std::wstring requestedMessage; bool amended = false;
        pane.requested = [&](const ResourceId& root, const std::wstring& message, bool amend) { ++requests; requestedRoot = root; requestedMessage = message; amended = amend; };
        pane.repositories({{first, L"first"}, {second, L"second"}});
        SetWindowTextW(input, L"日本語 \"message\"");
        SendMessageW(input, WM_IME_STARTCOMPOSITION, 0, 0); SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0);
        check(!requests, "IME confirmation submitted a commit");
        SendMessageW(input, WM_IME_ENDCOMPOSITION, 0, 0); SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0);
        check(requests == 1 && requestedRoot == first && requestedMessage == L"日本語 \"message\"" && !amended, "Enter did not submit the selected repository and message");
        pane.busy(true);
        SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(pane.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px(50), px(76)));
        SendMessageW(pane.window(), WM_LBUTTONUP, 0, MAKELPARAM(px(50), px(76)));
        check(requests == 1 && !IsWindowEnabled(input), "A pending commit allowed another submission");
        pane.busy(false);
        pane.repositories({{second, L"second"}}); SetWindowTextW(input, L"second draft");
        pane.repositories({{first, L"first"}});
        wchar_t text[128]{}; GetWindowTextW(input, text, 128);
        check(std::wstring(text) == requestedMessage, "A refresh, failed commit, or repository switch lost the first draft");
        pane.committed(first, requestedMessage); GetWindowTextW(input, text, 128);
        check(!*text, "A successful commit did not clear its message");
        pane.repositories({{second, L"second"}}); GetWindowTextW(input, text, 128);
        check(std::wstring(text) == L"second draft", "A successful commit cleared another repository's draft");
        pane.committed(second, L"older draft"); GetWindowTextW(input, text, 128);
        check(std::wstring(text) == L"second draft", "An older completion cleared a newer draft");
        SendMessageW(pane.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px(50), px(76)));
        SendMessageW(pane.window(), WM_LBUTTONUP, 0, MAKELPARAM(px(50), px(96)));
        check(requests == 1, "Releasing outside the commit button submitted a commit");
        SendMessageW(pane.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px(50), px(76)));
        SendMessageW(pane.window(), WM_LBUTTONUP, 0, MAKELPARAM(px(50), px(76)));
        check(requests == 2 && requestedRoot == second && !amended, "The commit button targeted the wrong repository");
        // The menu's nested message loop receives the same keys as a user.
        check(SetTimer(pane.window(), 99, 10, [](HWND window, UINT, UINT_PTR id, DWORD) {
            KillTimer(window, id); PostMessageW(window, WM_KEYDOWN, VK_HOME, 0); PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
        }) != 0, "Cannot schedule amend menu selection");
        SendMessageW(pane.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px(226), px(76)));
        SendMessageW(pane.window(), WM_LBUTTONUP, 0, MAKELPARAM(px(226), px(76)));
        check(requests == 3 && amended && requestedRoot == second && requestedMessage == L"second draft", "The dropdown did not submit Commit (Amend)");
        DestroyWindow(pane.window());
    }
    DestroyWindow(parent);
}
void drawCells(Renderer& renderer, Bitmap& bitmap, const std::vector<Cell>& cells, int cw, int ch) {
    check(renderer.begin(bitmap.bitmap.get()), "Could not bind Direct2D to the WIC bitmap");
    renderer.fill(bitmap.bounds(), background);
    renderer.cells(8, 8, cw, ch, cells, static_cast<int>(cells.size()), bitmap.bounds());
    finish(renderer); bitmap.read();
}
void cellPositions() {
    Renderer renderer, reference;
    Bitmap actual, expected;
    for (const UINT dpi : {96u, 144u, 192u}) {
        renderer.setDpi(dpi); reference.setDpi(dpi);
        const int cw = 24, ch = renderer.textHeight() + 4;
        std::vector<Cell> mixed(12), ascii(12);
        for (const auto index : {0, 3, 6, 9, 11}) mixed[index].ch = ascii[index].ch = U'A' + index;
        mixed[1].ch = U'日'; mixed[2].continuation = true;
        mixed[4].ch = U'\U0001f642'; mixed[5].continuation = true;
        mixed[7].ch = ascii[7].ch = U'f'; mixed[8].ch = ascii[8].ch = U'i';
        mixed[10].ch = U'\u05d0';
        drawCells(renderer, actual, mixed, cw, ch);
        drawCells(reference, expected, ascii, cw, ch);
        // Following glyphs must match an ASCII-only reference, even after a
        // fallback font, surrogate pair, RTL character, or potential ligature.
        for (const auto col : {0, 3, 6, 7, 8, 9, 11}) {
            bool ink = false;
            for (int y = 8; y < 8 + ch; ++y) for (int x = 8 + col * cw; x < 8 + (col + 1) * cw; ++x) {
                const auto pixel = expected.pixel(x, y);
                ink = ink || pixel != background;
                check(actual.pixel(x, y) == pixel, "Fallback text shifted a fixed-cell glyph or baseline");
            }
            check(ink, "DirectWrite did not draw the reference glyph");
        }
        // Brushes/targets are device resources; font metrics survive recreation.
        renderer.discard();
        drawCells(renderer, actual, ascii, cw, ch);
        for (int y = 0; y < Bitmap::height; ++y) for (int x = 0; x < Bitmap::width; ++x)
            check(actual.pixel(x, y) == expected.pixel(x, y), "Recreating the render target changed the drawing");
    }
}
void csvTables() {
    Viewer viewer;
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"CSV table test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1600, 1000, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create CSV table test owner");
    struct Owner { HWND window; ~Owner() { DestroyWindow(window); } } owner{parent};
    viewer.create(parent, instance); const auto window = viewer.window();
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    BYTE originalKeys[256]{}; GetKeyboardState(originalKeys);
    struct Keys { BYTE* value; ~Keys() { SetKeyboardState(value); } } keys{originalKeys};
    const auto key = [&](WPARAM code, bool ctrl = false, bool shift = false) {
        BYTE state[256]{}; state[VK_CONTROL] = ctrl ? 0x80 : 0; state[VK_SHIFT] = shift ? 0x80 : 0;
        SetKeyboardState(state); SendMessageW(window, WM_KEYDOWN, code, 0); SetKeyboardState(originalKeys);
    };
    const auto type = [&](const std::wstring& text) { for (const auto ch : text) SendMessageW(window, WM_CHAR, ch, 0); };
    for (const auto dpi : {96u, 144u, 192u}) {
        const auto px = [&](int value) { return MulDiv(value, dpi, 96); };
        viewer.setDpi(dpi); Renderer metrics; metrics.setDpi(dpi);
        MoveWindow(window, 0, 0, px(700), px(400), FALSE);
        const int rowHeight = std::max(px(28), metrics.textHeight() + px(8));
        const auto point = [&](int row, int column) { return MAKELPARAM(px(52 + column * 160 + 20), px(34) + rowHeight * (row + 1) + rowHeight / 2); };
        const auto click = [&](int row, int column) { SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(row, column)); SendMessageW(window, WM_LBUTTONUP, 0, point(row, column)); };
        auto buffer = std::make_shared<EditBuffer>();
        const std::string source = u8"name,qty,note\r\n豆腐,001,\"a,b\"\r\n豆,002,\"multi\r\nline\"\r\n";
        buffer->document.text = source; indexDocument(buffer->document); buffer->initialize(source);
        viewer.edit(buffer, L"table.CSV"); SetFocus(window);
        check(viewer.csvAvailable() && viewer.csvTable() && !buffer->dirty(), "CSV did not open as an unchanged table");
        check(viewer.textStatus().find(L"3 rows × 3 columns") != std::wstring::npos, "Multiline CSV has incorrect table dimensions");
        click(1, 1); check(viewer.state().caret == source.find("001"), "CSV cell hit testing did not follow DPI");
        key(VK_F2); key('A', true); type(L"1,2"); key(VK_RETURN);
        check(buffer->document.text == u8"name,qty,note\r\n豆腐,\"1,2\",\"a,b\"\r\n豆,002,\"multi\r\nline\"\r\n", "Cell editing failed to quote the value or changed unrelated cells");
        check(viewer.textStatus().find(L"B3") == 0, "Enter did not advance one CSV cell");
        key('Z', true); check(buffer->document.text == source && !buffer->dirty(), "Undo did not restore a complete cell edit");
        key('Y', true); check(buffer->dirty(), "Cannot redo a cell edit");
        click(1, 1); key(VK_F2); key('A', true); type(L"cancelled"); key(VK_ESCAPE);
        check(buffer->document.text.find("\"1,2\"") != std::string::npos, "Escape did not cancel the current cell edit");
        CsvTable table; check(table.parse(buffer->document.text), "Cell editing broke CSV syntax");
        viewer.restore({0, 0, table.offset({1, 0}), table.offset({2, 1})}); key('C', true);
        check(clipboardText(window) == L"豆腐\t1,2\r\n豆\t002", "CSV range copy is not Excel-compatible TSV");
        viewer.restore({}); check(copyText(window, L"A\tB\r\nC\t\"line1\r\nline2\""), "Cannot set Excel clipboard fixture"); key('V', true);
        check(table.parse(buffer->document.text) && table.value(buffer->document.text, {0, 0}) == "A" && table.value(buffer->document.text, {1, 1}) == "line1\r\nline2", "Rectangular Excel paste lost a multiline cell");
        key(VK_DELETE);
        check(table.parse(buffer->document.text) && table.value(buffer->document.text, {0, 0}).empty() && table.value(buffer->document.text, {1, 1}).empty(), "Delete did not clear the selected rectangle");
        click(0, 0); type(L"saved"); std::string saved;
        viewer.save = [&] { saved = buffer->document.text; }; key('S', true); viewer.save = {};
        check(saved.find("saved") == 0, "Saving omitted an active cell edit");
        viewer.setCsvTable(false); check(!viewer.csvTable() && viewer.buffer() == buffer, "Source mode lost the shared edit buffer");
        viewer.edit(buffer, L"table.CSV", true); check(!viewer.csvTable(), "CSV reload lost Source mode");
        viewer.setCsvTable(true); viewer.edit(buffer, L"table.CSV", true); check(viewer.csvTable(), "CSV reload lost Table mode");
        viewer.restore({});
        SCROLLINFO before{sizeof(before), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window, SB_HORZ, &before);
        const auto edge = MAKELPARAM(px(212), px(40));
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, edge);
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(px(292), px(40)));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(px(292), px(40)));
        SCROLLINFO after{sizeof(after), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window, SB_HORZ, &after);
        check(after.nMax == before.nMax + px(80), "CSV column resizing did not update the horizontal range");
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        viewer.find(); check(!viewer.csvTable(), "CSV search did not switch to searchable source");
        const auto find = FindWindowExW(window, nullptr, L"PoppyFindBar", nullptr); SendMessageW(find, WM_KEYDOWN, VK_ESCAPE, 0);
        viewer.setCsvTable(true); const auto selected = buffer->document.text.find("saved");
        check(viewer.selectSearch(selected, selected + 5, "saved", true) && !viewer.csvTable(), "Project search did not reveal CSV source");
        auto readonly = std::make_shared<Document>(buffer->document); viewer.show(readonly, L"readonly.csv");
        const auto unchanged = readonly->text; type(L"ignored"); key(VK_DELETE); check(readonly->text == unchanged, "Read-only CSV was edited");
        auto invalid = std::make_shared<Document>(); invalid->text = "\"unclosed"; indexDocument(*invalid); viewer.show(invalid, L"invalid.csv");
        check(viewer.csvTable() && viewer.textStatus().find(L"Invalid CSV") == 0, "Invalid CSV did not expose a recoverable table error");
        viewer.setCsvTable(false); check(!viewer.csvTable(), "Invalid CSV could not switch to Source");
        viewer.show(readonly, L"ordinary.txt"); check(!viewer.csvAvailable() && !viewer.csvTable(), "CSV mode leaked into a text file");
    }
}
void htmlPreviewSwitching() {
    Viewer viewer;
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"HTML preview test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 800, 500, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Could not create HTML preview test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, instance); MoveWindow(viewer.window(), 0, 0, 800, 500, FALSE);
    auto buffer = std::make_shared<EditBuffer>(); buffer->document.text = "<h1>before</h1>\r\n";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    const auto saved = buffer->document.text;
    // Fail deterministically before starting WebView2; no Runtime is required.
    const auto path = ResourceId::wsl("Ubuntu", "/site/bad\\name.html");
    viewer.edit(buffer, path);
    check(viewer.htmlAvailable() && viewer.htmlPreview(), "HTML did not open in Preview mode");
    viewer.edit(buffer, path, true);
    check(viewer.htmlPreview(), "Reloading HTML lost Preview mode");
    viewer.setHtmlPreview(false);
    viewer.edit(buffer, path, true);
    check(!viewer.htmlPreview(), "Reloading HTML lost the selected Source mode");
    SendMessageW(viewer.window(), WM_CHAR, 'X', 0);
    check(buffer->dirty(), "Could not edit HTML source before preview");
    const auto edited = buffer->document.text;
    const auto selection = viewer.state();
    for (const auto dpi : {96u, 144u, 192u}) {
        viewer.setDpi(dpi);
        // The removed toolbar must now accept selection on the first line.
        const auto point = MAKELPARAM(MulDiv(71, dpi, 96), MulDiv(1, dpi, 96));
        SendMessageW(viewer.window(), WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(viewer.window(), WM_LBUTTONUP, 0, point);
        check(!viewer.htmlPreview() && viewer.state().anchor == 0 && viewer.state().caret == 0,
            "The removed HTML toolbar still occupies space above the source");
        viewer.restore(selection);
        viewer.setHtmlPreview(true);
        check(viewer.htmlPreview(), "HTML preview did not activate");
        SendMessageW(viewer.window(), WM_CHAR, 'Y', 0);
        SendMessageW(viewer.window(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(WHEEL_DELTA)), 0);
        check(viewer.state().top == selection.top, "Scrolling an unavailable preview changed the source position");
        check(buffer->document.text == edited && viewer.buffer() == buffer, "Preview changed or released unsaved source");
        MoveWindow(viewer.window(), 0, 0, 760, 400, FALSE);
        viewer.setDpi(dpi);
        viewer.setHtmlPreview(false);
        check(!viewer.htmlPreview() && viewer.state().anchor == selection.anchor && viewer.state().caret == selection.caret,
            "Returning from a preview error lost the source selection");
        viewer.find();
        const auto input = FindWindowExW(viewer.window(), nullptr, L"PoppyFindBar", nullptr);
        RECT bounds{}; GetWindowRect(input, &bounds); MapWindowPoints(nullptr, viewer.window(), reinterpret_cast<POINT*>(&bounds), 2);
        check(bounds.top == 0, "The removed HTML toolbar still occupies space above source search");
        SendMessageW(viewer.window(), WM_KEYDOWN, VK_ESCAPE, 0);
        viewer.restore(selection);
    }
    size_t anchor = 0, caret = 0;
    check(buffer->undo(anchor, caret) && buffer->document.text == saved, "Preview discarded the source Undo history");
    viewer.refreshBuffer(); viewer.setHtmlPreview(true);
    viewer.edit(buffer, L"another.txt");
    check(!viewer.htmlAvailable() && !viewer.htmlPreview(), "Switching files left the HTML preview active");
    viewer.edit(buffer, path);
    check(viewer.htmlPreview(), "Reopening HTML did not default to Preview mode");
    viewer.showDiff(std::make_shared<DiffDocument>(workingDiff(buffer->document, buffer->document)), path);
    check(!viewer.htmlAvailable() && !viewer.htmlPreview(), "An HTML diff activated the preview");
    const auto revision = std::make_shared<Document>(buffer->document);
    viewer.showRevision(revision, DiffDocument{}, path);
    check(!viewer.htmlAvailable() && !viewer.htmlPreview() && !viewer.buffer(),
        "A historical HTML revision activated the current file's preview or retained an editable buffer");
    SendMessageW(viewer.window(), WM_CHAR, '!', 0);
    check(revision->text == buffer->document.text, "A historical revision accepted text input");
    viewer.edit(buffer, path, true);
    check(viewer.htmlPreview(), "Returning from an HTML diff did not default to Preview mode");
    viewer.message(L"Closed");
    check(!viewer.htmlPreview() && !viewer.hasDocument(), "Closing the document retained the HTML preview");
}
// Run explicitly with --html-preview-runtime on a Windows desktop with WebView2.
void htmlPreviewRuntime() {
    const auto directory = fs::temp_directory_path() / (L"Poppy WebView2 test " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
    check(fs::create_directory(directory), "Could not create WebView2 test files");
    struct Files { fs::path path; ~Files() { std::error_code error; fs::remove_all(path, error); } } files{directory};
    const auto first = directory / L"first.html", second = directory / L"second 日本語 #%.html";
    // If page scripts run, navigation to a missing file makes loaded() fail.
    for (const auto& path : {first, second}) {
        std::ofstream file(path); file << "<!doctype html><title>Preview reuse test</title><h1>" << path.u8string() << "</h1>"
            "<script>location.replace('script-must-not-run.html');</script>";
        check(file.good(), "Could not write WebView2 test HTML");
    }
    const auto parent = CreateWindowExW(0, L"STATIC", L"WebView2 reuse test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 800, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create WebView2 test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    HtmlPreview preview;
    const RECT bounds{0, 0, 800, 500};
    const auto pump = [] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    };
    const auto wait = [&](const auto& done) {
        const auto start = GetTickCount64();
        do {
            pump();
            if (done()) return;
            check(GetTickCount64() - start < 15000, "WebView2 did not finish starting or navigating");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        } while (true);
    };
    const auto loaded = [&] {
        wait([&] { return preview.status() != L"Loading HTML preview…"; });
        if (!preview.status().empty()) throw std::runtime_error(utf8(preview.status()));
        check(preview.active(), "Loading the page deactivated the preview");
    };
    // Finish initial creation after Source has already been selected.
    size_t changes = 0; preview.changed = [&] { ++changes; };
    preview.show(parent, first, bounds); preview.hide();
    const auto hiddenChanges = changes;
    wait([&] { const auto child = GetWindow(parent, GW_CHILD); return child && !IsWindowVisible(child); });
    const auto child = GetWindow(parent, GW_CHILD);
    check(!preview.active() && !IsWindowVisible(child), "Late WebView2 creation displayed a hidden preview");
    check(changes == hiddenChanges, "Late WebView2 creation notified an inactive preview");
    // A window handle alone could be recycled after destruction; its property cannot.
    const auto marker = reinterpret_cast<HANDLE>(1);
    check(SetPropW(child, L"PoppyWebView2ReuseTest", marker) != FALSE, "Could not mark the WebView2 controller window");
    const auto reused = [&] {
        check(GetWindow(parent, GW_CHILD) == child && GetPropW(child, L"PoppyWebView2ReuseTest") == marker,
            "Opening another HTML file recreated the WebView2 controller window");
    };
    preview.show(parent, second, bounds); loaded(); reused();
    preview.show(parent, directory / L"." / first.filename(), bounds); loaded(); reused();
    for (int i = 0; i < 3; ++i) {
        preview.hide();
        check(!preview.active() && !IsWindowVisible(child), "Source did not hide the retained WebView2 controller");
        reused(); preview.show(parent, first, bounds); loaded(); reused();
    }
    // Queue several switches without pumping messages; old completions must be ignored.
    preview.hide(); preview.show(parent, first, bounds); preview.hide();
    preview.show(parent, second, bounds); loaded(); reused();
    preview.show(parent, ResourceId::wsl("Ubuntu", "/site/bad\\name.html"), bounds);
    check(preview.active() && !preview.status().empty() && !IsWindowVisible(child), "An invalid path showed the previous page");
    reused(); preview.show(parent, second, bounds); loaded(); reused();
    preview.close();
    check(!preview.active(), "Closing WebView2 left the preview active");
    wait([&] { return GetWindow(parent, GW_CHILD) == nullptr; });
}
void colorsAndClipping() {
    Renderer renderer; renderer.setDpi(96);
    Bitmap bitmap;
    check(renderer.begin(bitmap.bitmap.get()), "Could not create the color test target");
    const COLORREF untouched = RGB(201, 12, 193);
    renderer.fill(bitmap.bounds(), untouched);
    std::vector<Cell> cells(4);
    for (int i = 0; i < 4; ++i) cells[i].bg = RGB(20 + i * 40, 30, 80);
    cells[0].ch = U'日'; cells[1].continuation = true;
    const RECT clip{12, 12, 62, 38};
    renderer.cells(4, 4, 20, 40, cells, 4, clip);
    finish(renderer); bitmap.read();
    for (int y = 0; y < Bitmap::height; ++y) for (int x = 0; x < Bitmap::width; ++x) {
        if (x < clip.left || x >= clip.right || y < clip.top || y >= clip.bottom)
            check(bitmap.pixel(x, y) == untouched, "Cell drawing escaped its clip");
    }
    // Inspect below the glyphs, including a wide-character continuation whose
    // background differs from the leading cell (partial terminal selection).
    for (int x = clip.left; x < clip.right; ++x)
        check(bitmap.pixel(x, 36) == cells[(x - 4) / 20].bg, "A cell background or continuation color was lost");
    check(!renderer.begin(static_cast<IWICBitmap*>(nullptr)), "A null bitmap should not begin drawing");
    Bitmap other;
    check(renderer.begin(other.bitmap.get()), "Could not switch to another WIC bitmap");
    renderer.fill(other.bounds(), accent);
    finish(renderer); other.read(); bitmap.read();
    check(other.pixel(0, 0) == accent && other.pixel(511, 127) == accent, "A replacement WIC bitmap was not rendered");
    check(bitmap.pixel(0, 0) == untouched, "Switching WIC targets changed the old bitmap");
}
void imagePreviews() {
    Renderer renderer; Bitmap bitmap;
    auto image = std::make_shared<ImagePreview>(); image->width = 2; image->height = 1;
    image->pixels = {0, 0, 255, 255, 0, 0, 0, 0};
    renderer.setImage(image);
    for (int pass = 0; pass < 2; ++pass) {
        check(renderer.begin(bitmap.bitmap.get()), "Could not create the image test target");
        renderer.fill(bitmap.bounds(), background);
        check(renderer.drawImage({10, 10, 30, 20}, 4), "Could not render image pixels");
        finish(renderer); bitmap.read();
        check(bitmap.pixel(19, 14) == RGB(255, 0, 0), "Opaque image color was lost");
        check(bitmap.pixel(20, 14) == RGB(68, 74, 84), "Transparent image pixel hid the checkerboard");
        check(bitmap.pixel(18, 14) == background && bitmap.pixel(21, 14) == background, "Image escaped its fitted rectangle");
        renderer.discard(); // Device loss must recreate the bitmap from CPU pixels.
    }
    HWND parent = CreateWindowExW(0, L"STATIC", L"Image viewer test", WS_POPUP, 0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create an image viewer parent");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    Viewer viewer; viewer.create(parent, GetModuleHandleW(nullptr));
    SetWindowPos(viewer.window(), nullptr, 0, 0, 320, 200, SWP_NOZORDER);
    auto buffer = std::make_shared<EditBuffer>();
    buffer->document.text = std::string(1000, '\n'); indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"before.txt"); viewer.find();
    viewer.showImage(image, ResourceId(fs::path(L"preview.png")));
    check(viewer.hasDocument() && !viewer.buffer(), "Image did not replace the editable document");
    check(viewer.textStatus() == L"2 × 1 px", "Image status retained the previous text encoding");
    SendMessageW(viewer.window(), WM_CHAR, 'x', 0); viewer.find();
    check(!viewer.buffer() && viewer.state().caret == 0, "Image accepted text editing or selection");
    viewer.setDpi(192); SetWindowPos(viewer.window(), nullptr, 0, 0, 90, 90, SWP_NOZORDER);
    SendMessageW(viewer.window(), WM_PAINT, 0, 0);
    check(viewer.documentBytes() >= image->pixels.size(), "Image was omitted from memory accounting");
    const std::weak_ptr<const ImagePreview> lifetime = image;
    viewer.edit(buffer, L"after.txt");
    check(viewer.buffer() == buffer, "Switching from an image did not restore text editing");
    viewer.showImage(image, ResourceId(fs::path(L"preview.png"))); image.reset();
    viewer.message(L"Closed"); renderer.setImage({});
    check(!viewer.hasDocument() && viewer.documentBytes() == 0 && lifetime.expired(), "Closing an image retained its pixels");
}
void fractionalMarkers() {
    Renderer renderer; renderer.setDpi(96); Bitmap bitmap;
    const auto draw = [&](float top) {
        check(renderer.begin(bitmap.bitmap.get()), "Could not create the fractional marker test target");
        renderer.fill(bitmap.bounds(), RGB(0, 0, 0));
        renderer.fill(10.0f, top, 14.0f, top + 3.0f, RGB(255, 255, 255));
        renderer.fill({40, 30, 44, 34}, accent);
        finish(renderer); bitmap.read();
    };
    draw(30.25f); const int first = GetRValue(bitmap.pixel(11, 30));
    draw(30.75f); const int next = GetRValue(bitmap.pixel(11, 30));
    check(first < 255 && next > 0 && next < first, "Fractional marker motion was rounded to whole pixels");
    check(bitmap.pixel(11, 31) == RGB(255, 255, 255) && bitmap.pixel(11, 29) == RGB(0, 0, 0) && bitmap.pixel(41, 31) == accent,
        "Fractional marker drawing changed its solid interior or an unrelated region");
}
void smoothIcons() {
    Renderer renderer; Bitmap bitmap;
    for (const UINT dpi : {96u, 120u, 144u, 192u}) {
        renderer.setDpi(dpi);
        check(renderer.begin(bitmap.bitmap.get()), "Could not create the icon test target");
        renderer.fill(bitmap.bounds(), RGB(0, 0, 0));
        const int size = MulDiv(24, dpi, 96);
        const std::vector<Renderer::Icon> icons{Renderer::Icon::Explorer, Renderer::Icon::SourceControl, Renderer::Icon::Branch,
            Renderer::Icon::OpenAllChanges, Renderer::Icon::StageChanges, Renderer::Icon::UnstageChanges, Renderer::Icon::DiscardChanges, Renderer::Icon::StashChanges, Renderer::Icon::OpenFile,
            Renderer::Icon::Eye, Renderer::Icon::EyeClosed, Renderer::Icon::CollapseAll, Renderer::Icon::OpenRevision, Renderer::Icon::Close};
        for (size_t i = 0; i < icons.size(); ++i) {
            const int x = 8 + static_cast<int>(i % 9) * 56, y = 8 + static_cast<int>(i / 9) * 56;
            renderer.icon(icons[i], {x, y, x + size, y + size}, RGB(255, 255, 255));
        }
        // Icon transforms must not leak into subsequent panel/cell fills.
        renderer.fill({440, 80, 460, 100}, accent);
        finish(renderer); bitmap.read();
        for (size_t i = 0; i < icons.size(); ++i) {
            const int first = 8 + static_cast<int>(i % 9) * 56, top = 8 + static_cast<int>(i / 9) * 56;
            bool ink = false, blendedEdge = false;
            for (int y = top - 1; y <= top + size; ++y) for (int x = first - 1; x <= first + size; ++x) {
                const auto pixel = bitmap.pixel(x, y);
                ink = ink || pixel != RGB(0, 0, 0);
                blendedEdge = blendedEdge || (pixel != RGB(0, 0, 0) && pixel != RGB(255, 255, 255));
            }
            check(ink && blendedEdge, "A vector icon is missing or has jagged, unblended edges");
        }
        for (int y = 79; y < 101; ++y) for (int x = 439; x < 461; ++x)
            check(bitmap.pixel(x, y) == (x >= 440 && x < 460 && y >= 80 && y < 100 ? accent : RGB(0, 0, 0)),
                "Icon scaling changed a subsequent fill or blurred an integer-aligned panel edge");
    }
}
void windowTargets() {
    HWND window = CreateWindowExW(0, L"STATIC", L"Poppy render test", WS_POPUP, 0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "Could not create a hidden render-test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{window};
    Renderer renderer; renderer.setDpi(96);
    const int width = renderer.cellWidth(), height = renderer.textHeight();
    renderer.setDpi(192);
    check(renderer.cellWidth() > width && renderer.textHeight() > height, "DPI changes did not update DirectWrite metrics");
    for (const auto size : {SIZE{320, 200}, SIZE{640, 360}, SIZE{0, 0}, SIZE{320, 200}}) {
        check(SetWindowPos(window, nullptr, 0, 0, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not resize the render-test window");
        const bool drawing = renderer.begin(window);
        check(drawing == (size.cx > 0 && size.cy > 0), "Window resize or zero-size handling failed");
        if (drawing) {
            renderer.fill({0, 0, size.cx, size.cy}, panel);
            renderer.label({8, 8, size.cx - 8, 80}, L"DirectWrite 日本語", foreground);
            finish(renderer);
        }
    }
    Bitmap bitmap;
    drawCells(renderer, bitmap, {Cell{U'M', foreground, background, false}}, 24, renderer.textHeight() + 4);
    check(renderer.begin(window), "Could not switch from a WIC target back to an HWND target");
    renderer.fill({0, 0, 320, 200}, accent);
    finish(renderer);
    renderer.discard();
    check(renderer.begin(window), "Could not recreate the HWND render target");
    finish(renderer);
}
void editableDiff() {
    Viewer viewer;
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Editable diff test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 960, 600, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Could not create editable diff test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, instance); MoveWindow(viewer.window(), 0, 0, 960, 600, FALSE);
    BYTE original[256]{}; GetKeyboardState(original);
    struct KeyboardOwner { BYTE* value; ~KeyboardOwner() { SetKeyboardState(value); } } keyboard{original};
    const auto key = [&](WPARAM value, bool ctrl = false, bool shift = false, bool alt = false) {
        BYTE keys[256]{}; keys[VK_CONTROL] = ctrl ? 0x80 : 0; keys[VK_SHIFT] = shift ? 0x80 : 0; keys[VK_MENU] = alt ? 0x80 : 0;
        SetKeyboardState(keys); SendMessageW(viewer.window(), alt ? WM_SYSKEYDOWN : WM_KEYDOWN, value, 0); SetKeyboardState(original);
    };
    auto before = std::make_shared<Document>(); before->text = "head\r\nold\r\ntail"; before->language = Language::CLike; indexDocument(*before);
    for (const bool inlineView : {false, true}) for (const bool wrap : {false, true}) for (const UINT dpi : {96u, 144u, 192u}) {
        auto buffer = std::make_shared<EditBuffer>(); buffer->document.text = "head\r\n日本😀\r\ntail";
        indexDocument(buffer->document); buffer->initialize(buffer->document.text);
        viewer.setDpi(dpi); viewer.setWordWrap(wrap); viewer.setDiffLayout(inlineView);
        viewer.editDiff(buffer, before, L"file.cpp"); SetFocus(viewer.window());
        const auto select = [&](size_t anchor, size_t caret) {
            const auto diff = workingDiff(*before, buffer->document);
            viewer.restore({0, 0, diff.patchOffset(anchor), diff.patchOffset(caret), true});
        };
        select(6, 16); SendMessageW(viewer.window(), WM_CHAR, 'x', 0);
        check(buffer->document.text == "head\r\nx\r\ntail", "Editing a diff selection used patch offsets or corrupted CRLF");
        key('Z', true); check(!buffer->dirty(), "Undo in a diff did not restore the saved working text");
        key('Y', true); check(buffer->document.text == "head\r\nx\r\ntail", "Redo in a diff lost the working edit");
        select(7, 7); key(VK_DELETE);
        check(buffer->document.text == "head\r\nxtail", "Delete in a diff did not join source lines");
        key('Z', true); select(9, 9); key(VK_BACK);
        check(buffer->document.text == "head\r\nxtail", "Backspace in a diff split CRLF or targeted a patch prefix");
        key('Z', true); select(7, 7); key(VK_BACK, true);
        check(buffer->document.text == "head\r\n\r\ntail", "Ctrl+Backspace in a diff did not delete the source word");
        key('Z', true); select(6, 6); key(VK_BACK, true);
        check(buffer->document.text == "x\r\ntail", "Ctrl+Backspace across a diff line split CRLF or deleted patch metadata");
        key('Z', true); select(6, 6); key('X', true);
        check(buffer->document.text == "head\r\ntail" && clipboardText(viewer.window()) == L"x\r\n", "Cut in a diff did not use the working source line");
        key('Z', true); select(6, 6); key(VK_DOWN, false, true, true);
        check(buffer->document.text == "head\r\ntail\r\nx", "Moving a diff line used patch metadata"); key('Z', true);
        key('A', true); SendMessageW(viewer.window(), WM_CHAR, 'z', 0);
        check(buffer->document.text == "z", "Select All in a diff did not replace the entire working file"); key('Z', true);
        const auto diff = workingDiff(*before, buffer->document);
        for (const auto& row : diff.rows) if (row.changed && row.before.number) {
            viewer.restore({0, 0, diff.offset(row.before), diff.offset(row.before), false}); break;
        }
        const auto text = buffer->document.text;
        SendMessageW(viewer.window(), WM_CHAR, '!', 0); key(VK_DELETE); key(VK_BACK, true); key('Z', true);
        check(buffer->document.text == text, "The Before side of a working diff accepted an edit");
        select(6, 7);
        auto changedBase = std::make_shared<Document>(*before); changedBase->text = "different\r\nhead\r\nold\r\ntail"; indexDocument(*changedBase);
        viewer.editDiff(buffer, changedBase, L"file.cpp", true); SendMessageW(viewer.window(), WM_CHAR, 'q', 0);
        check(buffer->document.text == "head\r\nq\r\ntail", "Refreshing the index moved a working selection to a different file offset");
        int saves = 0; viewer.save = [&] { ++saves; }; key('S', true); check(saves == 1, "Ctrl+S in a diff did not request a save"); viewer.save = {};
        viewer.showDiff(std::make_shared<DiffDocument>(diff), L"file.cpp"); SendMessageW(viewer.window(), WM_CHAR, '!', 0);
        check(!viewer.buffer() && buffer->document.text == "head\r\nq\r\ntail", "Read-only comparisons retained an editable buffer");

        const std::string indented = u8"    head\r\n\t日本😀\r\n  tail";
        buffer->document.text = indented; indexDocument(buffer->document); buffer->initialize(indented);
        viewer.editDiff(buffer, before, L"file.cpp"); select(indented.size(), 0);
        key(VK_TAB, false, true);
        check(buffer->document.text == u8"head\r\n日本😀\r\ntail", "Shift+Tab in a diff used patch offsets");
        const auto outdented = workingDiff(*before, buffer->document);
        check(viewer.state().anchor == outdented.patchOffset(buffer->document.text.size()) && viewer.state().caret == outdented.patchOffset(0),
            "Diff outdent lost the selection direction");
        key('Z', true); check(buffer->document.text == indented && !buffer->dirty(), "Diff outdent did not undo as one edit");
        const auto restored = workingDiff(*before, buffer->document);
        for (const auto& row : restored.rows) if (row.before.number) {
            viewer.restore({0, 0, restored.offset(row.before), restored.offset(row.before), false}); break;
        }
        key(VK_TAB, false, true);
        check(buffer->document.text == indented, "Shift+Tab edited the Before side of a diff");
    }
    auto buffer = std::make_shared<EditBuffer>(); buffer->document.text = std::string(1000, 'x'); indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.setWordWrap(true); viewer.setDpi(96); viewer.setDiffLayout(false); viewer.editDiff(buffer, before, L"file.cpp");
    key(VK_END); const auto end = viewer.state(); key(VK_HOME); key(VK_END);
    check(viewer.state().caret == end.caret, "End/Home crossed an editable diff wrap boundary");
    key(VK_END, true); SendMessageW(viewer.window(), WM_CHAR, '\r', 0); SendMessageW(viewer.window(), WM_CHAR, 0x65e5, 0);
    check(buffer->document.text == std::string(1000, 'x') + "\n日", "Typing at the wrapped diff EOF lost a newline or Unicode text");
}
void editorCaret() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Editor caret test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the editor test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr)); viewer.setDpi(96);
    const HWND window = viewer.window();
    check(MoveWindow(window, 0, 0, 640, 360, FALSE) != FALSE, "Could not size the editor");
    auto buffer = std::make_shared<EditBuffer>();
    indexDocument(buffer->document); buffer->initialize("");
    viewer.edit(buffer, L"caret.txt"); SetFocus(window);
    check(GetFocus() == window, "Could not focus the editor");
    const auto caret = [&] {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        check(GetGUIThreadInfo(GetCurrentThreadId(), &info) != FALSE, "Could not inspect the editor caret");
        check(info.hwndCaret == window, "The editor lost its native caret for IME/accessibility");
        check(!(info.flags & GUI_CARETBLINKING), "The native XOR caret is still drawing over Direct2D");
        return info.rcCaret;
    };
    const auto initial = caret();
    check(initial.top == 0 && viewer.textStatus() == L"UTF-8  ·  LF", "The removed header still occupies editor space or empty-file status is wrong");
    const LONG height = initial.bottom - initial.top;
    for (int row = 1; row <= 4; ++row) {
        SendMessageW(window, WM_CHAR, '\r', 0);
        SendMessageW(window, WM_PAINT, 0, 0);
        const auto current = caret();
        check(current.left == initial.left && current.top == initial.top + row * height,
            "A newline left the caret on the previous line");
    }
    for (int row = 0; row < 40; ++row) {
        SendMessageW(window, WM_CHAR, '\r', 0);
        SendMessageW(window, WM_PAINT, 0, 0);
        const auto current = caret();
        check(current.top >= initial.top && current.bottom <= Scrollbars::contentRect(window).bottom,
            "Newlines scrolled the caret outside the editor body");
    }
    check(viewer.state().caret == buffer->document.text.size(), "Repeated newlines lost the insertion point");
    SendMessageW(window, WM_KEYDOWN, VK_BACK, 0); caret();
    SendMessageW(window, WM_KEYDOWN, VK_UP, 0); caret();
    viewer.setDpi(144);
    check(caret().bottom - caret().top > height, "DPI changes did not resize the editor caret");

    // Use a real timer message so this also verifies that blinking was started.
    const UINT blink = GetCaretBlinkTime();
    MSG timer{};
    if (blink != INFINITE && blink <= 5000) {
        const auto start = GetTickCount64();
        while (!PeekMessageW(&timer, window, WM_TIMER, WM_TIMER, PM_REMOVE)) {
            check(GetTickCount64() - start < (blink ? blink : 500) + 1000, "The editor did not start its caret blink timer");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_TIMER);
        }
        for (int phase = 0; phase < 2; ++phase) {
            ValidateRect(window, nullptr); DispatchMessageW(&timer);
            check(GetUpdateRect(window, nullptr, FALSE) != FALSE, "Caret blinking did not request a Direct2D repaint");
            SendMessageW(window, WM_PAINT, 0, 0); caret();
        }
    }
    SetFocus(parent);
    check(GetUpdateRect(window, nullptr, FALSE) != FALSE, "Leaving the editor did not repaint its visible caret");
    ValidateRect(window, nullptr);
    if (timer.hwnd) DispatchMessageW(&timer);
    check(!GetUpdateRect(window, nullptr, FALSE), "A stale blink timer repainted an unfocused editor");
    SetFocus(window); caret();
    viewer.show(std::make_shared<Document>(buffer->document), L"read-only.txt");
    ValidateRect(window, nullptr);
    if (timer.hwnd) DispatchMessageW(&timer);
    check(!GetUpdateRect(window, nullptr, FALSE), "A stale blink timer repainted a read-only viewer");
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret != window,
        "A read-only viewer retained the editor caret");
    auto crlf = std::make_shared<EditBuffer>();
    crlf->document.text = "first\r\nsecond\n"; crlf->document.encoding = "UTF-16 LE";
    indexDocument(crlf->document); crlf->initialize(crlf->document.text);
    int updates = 0; viewer.statusChanged = [&] { ++updates; };
    viewer.edit(crlf, L"crlf.txt");
    check(viewer.textStatus() == L"UTF-16 LE  ·  CRLF", "Status did not use the source encoding and first line ending");
    viewer.restore({0, 0, 5, 7}); SetFocus(window); SendMessageW(window, WM_KEYDOWN, VK_DELETE, 0);
    check(viewer.textStatus() == L"UTF-16 LE  ·  LF" && updates >= 2, "Editing the first newline did not refresh line-ending status");
    viewer.message(L"Loading");
    check(viewer.textStatus().empty(), "A loading message retained the previous file format");
}
void delayedLoadingMessages() {
    Viewer pending;
    std::array<Viewer, 6> completed;
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Loading message test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the loading message test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    constexpr UINT_PTR loadingTimer = 0x5059;
    auto buffer = std::make_shared<EditBuffer>();
    buffer->document.text = "original"; buffer->document.encoding = "UTF-8";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    for (auto& viewer : completed) {
        viewer.create(parent, instance); MoveWindow(viewer.window(), 0, 0, 640, 360, FALSE);
        viewer.edit(buffer, L"old.txt"); viewer.loading(L"Loading…", L"next.txt");
        check(!viewer.hasDocument() && !viewer.buffer() && viewer.textStatus().empty(),
            "Waiting to show loading text retained the previous editable document or status");
        SendMessageW(viewer.window(), WM_CHAR, 'x', 0);
        check(buffer->document.text == "original", "Typing while loading edited the previous tab");
    }
    completed[0].edit(buffer, L"next.txt");
    completed[1].show(std::make_shared<Document>(buffer->document), L"next.txt");
    auto diff = std::make_shared<DiffDocument>();
    diff->patch.text = "diff --git a/next.txt b/next.txt\n--- a/next.txt\n+++ b/next.txt\n@@ -1 +1 @@\n-old\n+new\n";
    indexDiff(*diff, true); completed[2].showDiff(diff, L"next.txt");
    auto image = std::make_shared<ImagePreview>(); image->width = image->height = 1; image->pixels = {0, 0, 0, 255};
    completed[3].showImage(image, L"next.png");
    completed[4].message(L"Cannot read this file.", L"next.txt");
    completed[5].message(L"Select a file.");

    pending.create(parent, instance); MoveWindow(pending.window(), 0, 0, 640, 360, FALSE);
    pending.loading(L"Loading…", L"first.txt");
    ValidateRect(pending.window(), nullptr);
    SendMessageW(pending.window(), WM_TIMER, loadingTimer, 0);
    check(!GetUpdateRect(pending.window(), nullptr, FALSE), "Loading text appeared before one second");
    const auto waitTimer = [&] {
        const auto start = GetTickCount64();
        MSG timer{};
        while (GetTickCount64() - start < 3000) {
            if (PeekMessageW(&timer, pending.window(), WM_TIMER, WM_TIMER, PM_REMOVE)) {
                if (timer.wParam == loadingTimer) return timer;
                DispatchMessageW(&timer);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_TIMER);
        }
        throw std::runtime_error("The loading message timer was not started");
    };
    // An expired notification can remain queued when another file is opened.
    const auto previous = waitTimer();
    const auto started = GetTickCount64();
    pending.loading(L"Loading Git repository…", L"second.txt");
    ValidateRect(pending.window(), nullptr); DispatchMessageW(&previous);
    check(!GetUpdateRect(pending.window(), nullptr, FALSE), "The previous tab's timer shortened the next tab's delay");
    while (!GetUpdateRect(pending.window(), nullptr, FALSE)) {
        auto timer = waitTimer();
        pending.loading(L"Loading Git diff…", L"second.txt");
        DispatchMessageW(&timer);
        check(GetTickCount64() - started < 3000, "Changing loading phases restarted the same file's delay");
    }
    check(GetTickCount64() - started >= 1000, "A slow read showed its message before one second");
    ValidateRect(pending.window(), nullptr);
    SendMessageW(pending.window(), WM_TIMER, loadingTimer, 0);
    check(!GetUpdateRect(pending.window(), nullptr, FALSE), "A visible loading message kept repainting on stale timers");
    for (auto& viewer : completed) {
        ValidateRect(viewer.window(), nullptr);
        SendMessageW(viewer.window(), WM_TIMER, loadingTimer, 0);
        check(!GetUpdateRect(viewer.window(), nullptr, FALSE), "A completed read or closed tab retained a loading notification");
    }
}
void imeCellRendering() {
    Renderer renderer;
    Bitmap actual, expected;
    for (const UINT dpi : {96u, 120u, 144u, 192u}) {
        renderer.setDpi(dpi);
        const int cw = renderer.cellWidth(), ch = renderer.textHeight() + MulDiv(2, dpi, 96);
        const int columns = (Bitmap::width - 16) / cw;
        const RECT clip{8, 8, 8 + columns * cw, std::min(Bitmap::height - 8, 8 + 3 * ch)};
        for (const int first : {2, columns - 1}) {
            CompositionText text{u"iWいい、り漢字ｱ😀Z", {}, 0}; text.cursor = text.text.size();
            const POINT origin{8 + first * cw, 8};
            const auto cursor = renderer.compositionCaret(text, origin, cw, ch, clip);
            check(renderer.begin(actual.bitmap.get()), "Cannot render fixed-cell IME text");
            renderer.fill(actual.bounds(), panel); renderer.fill(clip, background);
            renderer.composition(text, origin, cw, ch, clip); finish(renderer); actual.read();
            // Feed the same text through the terminal, including wrapping at the
            // right edge. Glyph pixels must agree without relying on font names
            // or the LOGFONT average width.
            TerminalModel terminal(columns, 3);
            terminal.feed("\x1b[1;" + std::to_string(first + 1) + "H" + u8"iWいい、り漢字ｱ😀Z");
            check(renderer.begin(expected.bitmap.get()), "Cannot render the terminal IME reference");
            renderer.fill(expected.bounds(), panel); renderer.fill(clip, background);
            for (int row = 0; row < 3; ++row) renderer.cells(8, 8 + row * ch, cw, ch, terminal.row(row), columns, clip);
            finish(renderer); expected.read();
            bool ink = false;
            for (int y = 0; y < Bitmap::height; ++y) for (int x = 0; x < Bitmap::width; ++x) {
                if (x >= clip.left && x < clip.right && y >= clip.top && y < clip.bottom) {
                    if ((y - clip.top) % ch >= ch - 2) continue; // Composition underline.
                    if (x >= cursor.x && x < cursor.x + 2 && y >= cursor.y && y < cursor.y + ch) continue;
                    ink |= expected.pixel(x, y) != background;
                }
                check(actual.pixel(x, y) == expected.pixel(x, y), "IME preedit glyphs differ from committed fixed-cell terminal text");
            }
            check(ink, "No IME reference glyphs were drawn");
        }
        CompositionText target{u"日あ", {ATTR_TARGET_CONVERTED, ATTR_INPUT}, 2};
        check(renderer.begin(actual.bitmap.get()), "Cannot render IME clause attributes");
        renderer.fill(actual.bounds(), background); renderer.composition(target, {8, 8}, cw, ch, clip); finish(renderer); actual.read();
        for (int x = 8; x < 8 + 4 * cw; ++x)
            check(actual.pixel(x, 8 + ch - 1) == (x < 8 + 2 * cw ? accent : muted), "IME conversion target underline does not follow its fixed cells");
        const auto cursor = renderer.compositionCaret(target, {8, clip.bottom - ch}, cw, ch, {8, 8, 8 + 3 * cw, clip.bottom});
        check(cursor.x == 8 + 2 * cw && cursor.y + ch <= clip.bottom, "Wrapped IME candidates would leave the text viewport");
    }
}
void imeTerminalInsertion() {
    Renderer renderer;
    Bitmap actual, expected;
    for (const UINT dpi : {96u, 120u, 144u, 192u}) {
        renderer.setDpi(dpi);
        const int cw = renderer.cellWidth(), ch = renderer.textHeight() + MulDiv(2, dpi, 96);
        for (const bool coloredPanel : {false, true}) for (const auto& text : {CompositionText{u"にほんご", {}, 4}, CompositionText{u"日本語", {}, 1}, CompositionText{u"日😀ｱ", {}, 2}}) {
            const int columns = coloredPanel ? 28 : 18, rows = 2;
            const RECT clip{8, 8, 8 + columns * cw, 8 + rows * ch};
            const std::string suffix = u8"日本😀defghijk";
            const std::string color = "\x1b[38;2;70;180;210m";
            const std::string base = coloredPanel ? "\x1b[48;2;30;40;50m\x1b[2K" : "";
            TerminalModel model(columns, rows), reference(columns, rows); TerminalDisplay display;
            model.feed(base + "abc" + color + suffix + "\x1b[0m\x1b[1;4H"); display.update(model, 0, 0, true);
            const auto preview = display.preedit(text);
            reference.feed(base + "abc" + utf8(std::wstring(text.text.begin(), text.text.end())) + color + suffix);
            check(renderer.begin(actual.bitmap.get()), "Cannot render terminal preedit insertion");
            renderer.fill(actual.bounds(), panel); renderer.fill(clip, background);
            for (int y = 0; y < rows; ++y) renderer.cells(8, 8 + y * ch, cw, ch, preview.cells[y], columns, clip);
            renderer.composition(preview.composition, {8, 8}, cw, ch, clip, preview.compositionBackground); finish(renderer); actual.read();
            check(renderer.begin(expected.bitmap.get()), "Cannot render terminal insertion reference");
            renderer.fill(expected.bounds(), panel); renderer.fill(clip, background);
            for (int y = 0; y < rows; ++y) renderer.cells(8, 8 + y * ch, cw, ch, reference.row(y), columns, clip);
            finish(renderer); expected.read();
            const int caretX = 8 + preview.composition.caretColumn * cw, caretY = 8 + preview.composition.caretRow * ch;
            for (int y = 0; y < Bitmap::height; ++y) for (int x = 0; x < Bitmap::width; ++x) {
                if (x >= clip.left && x < clip.right && y >= clip.top && y < clip.bottom) {
                    if ((y - clip.top) % ch >= ch - 2) continue;
                    if (x >= caretX && x < caretX + 2 && y >= caretY && y < caretY + ch) continue;
                }
                check(actual.pixel(x, y) == expected.pixel(x, y), "Preedit and shifted suffix differ from committed terminal insertion");
            }
        }
    }
}
void imeUiRendering() {
    Renderer renderer, reference;
    Bitmap actual, expected;
    for (const UINT dpi : {96u, 120u, 144u, 192u}) {
        renderer.setDpi(dpi); reference.setDpi(dpi);
        const RECT clip{8, 8, 500, 8 + MulDiv(32, dpi, 96)};
        const std::wstring committed = L"prefix old.txt";
        CompositionText composition{u"iW日本語ｱ😀Z", {}, 5};
        const auto before = committed;
        InputLine line;
        line.update(renderer, committed, 10, 7, composition, clip);
        const auto caret = line.caret(); const int bottom = caret.y + line.height();
        check(committed == before, "Drawing preedit changed committed UI input");
        check(renderer.begin(actual.bitmap.get()), "Cannot draw proportional preedit");
        renderer.fill(actual.bounds(), background); line.draw(renderer, foreground, panel, false); finish(renderer); actual.read();
        const auto display = L"prefix " + std::wstring(composition.text.begin(), composition.text.end()) + L".txt";
        check(reference.begin(expected.bitmap.get()), "Cannot draw proportional reference");
        reference.fill(expected.bounds(), background);
        reference.label(clip, display, foreground, DT_LEFT | DT_VCENTER | DT_SINGLELINE); finish(reference); expected.read();
        bool ink = false;
        for (int y = 0; y < Bitmap::height; ++y) for (int x = 0; x < Bitmap::width; ++x) {
            if (y >= bottom - 2 && y < bottom) continue; // The preedit underline.
            check(actual.pixel(x, y) == expected.pixel(x, y), "UI preedit glyphs differ from the committed proportional text");
            ink |= actual.pixel(x, y) != background;
        }
        check(ink, "The proportional IME text is empty");
        // Verify the cursor against DirectWrite independently, including the
        // preceding text, fallback glyphs and UTF-16 cursor offset.
        ComPtr<IDWriteFactory> factory; ComPtr<IDWriteTextFormat> font; ComPtr<IDWriteTextLayout> layout;
        check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.put()))), "Cannot create the UI IME reference factory");
        check(SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f * dpi / 96, L"", font.put())), "Cannot create the UI IME reference font");
        check(SUCCEEDED(factory->CreateTextLayout(display.data(), static_cast<UINT32>(display.size()), font.get(),
            static_cast<FLOAT>(clip.right - clip.left), static_cast<FLOAT>(clip.bottom - clip.top), layout.put())), "Cannot create the UI IME reference layout");
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        FLOAT x = 0, y = 0; DWRITE_HIT_TEST_METRICS hit{};
        layout->HitTestTextPosition(static_cast<UINT32>(7 + composition.cursor), FALSE, &x, &y, &hit);
        check(caret.x == clip.left + std::lround(x) && caret.y == clip.top + std::lround(y), "UI IME caret does not follow its shaped UTF-16 text position");
        composition = {u"iiii", {}, 4}; line.update(renderer, L"", 0, 0, composition, clip); const auto narrow = line.caret().x - clip.left;
        composition = {u"WWWW", {}, 4}; line.update(renderer, L"", 0, 0, composition, clip);
        check(line.caret().x - clip.left > 2 * narrow, "UI IME text was laid out in fixed cells");
        composition = {u"iW", {ATTR_INPUT, ATTR_TARGET_CONVERTED}, 2}; line.update(renderer, L"", 0, 0, composition, clip);
        const auto underline = line.caret().y + line.height() - 1;
        check(renderer.begin(actual.bitmap.get()), "Cannot draw proportional clause attributes");
        renderer.fill(actual.bounds(), background); line.draw(renderer, foreground, panel, false); finish(renderer); actual.read();
        check(actual.pixel(clip.left + 1, underline) == muted && actual.pixel(line.caret().x - 2, underline) == accent,
            "UI IME clause underlines do not follow proportional widths");
        composition = {u"😀", {}, 1}; line.update(renderer, L"", 0, 0, composition, clip);
        const auto insideSurrogate = line.caret();
        composition.cursor = 0; line.update(renderer, L"", 0, 0, composition, clip);
        check(insideSurrogate.x == line.caret().x, "UI IME cursor split a surrogate pair");
        const std::wstring clusters = L"iW😀a\u0301z";
        line.update(renderer, clusters, 0, 0, {}, clip);
        for (int px = clip.left; px < clip.left + renderer.textWidth(clusters); ++px) {
            const auto position = line.hit(px);
            check(position != 3 && position != 5, "UI hit testing split a UTF-16 or combining cluster");
        }
        const RECT narrowClip{8, 8, 80, clip.bottom};
        composition = {std::u16string(80, u'W') + u"日本語😀", {}, 85};
        line.update(renderer, L"prefix.txt", 6, 6, composition, narrowClip);
        check(line.caret().x >= narrowClip.left && line.caret().x < narrowClip.right, "Long UI preedit did not scroll its cursor into view");
        composition.cursor = 0; line.update(renderer, L"prefix.txt", 6, 6, composition, narrowClip);
        check(line.caret().x >= narrowClip.left && line.caret().x < narrowClip.right, "Moving the preedit cursor left escaped the input");
        line.update(renderer, L"short", 5, 5, {}, clip);
        check(std::abs(line.caret().x - clip.left - renderer.textWidth(L"short")) <= 1, "Cancelling preedit retained its horizontal scroll");
    }
}
void imePositions() {
    Viewer viewer;
    Terminal terminal, backgroundTerminal;
    QuickOpen quickOpen;
    FindBar projectFind; TreeInput rename; Tree navigator;
    int treeActivations = 0;
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"IME position test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 900, 700, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the IME test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, instance); terminal.create(parent, instance); backgroundTerminal.create(parent, instance);
    quickOpen.create(parent, instance, QuickOpenMode::Projects);
    projectFind.create(parent, instance); projectFind.projectSearch(); projectFind.setDpi(96);
    projectFind.layout(0, 600, 64); projectFind.show(L"iW日本語", false);
    rename.setDpi(96); const auto renameWindow = rename.create(parent, instance, L"iW日本語.txt", foreground);
    MoveWindow(renameWindow, 0, 0, 400, 32, FALSE);
    navigator.create(parent, instance, L"Type to select", 55);
    navigator.activated = [&](TreeItem) { ++treeActivations; };
    SetWindowPos(navigator.window(), nullptr, 0, 0, 400, 160, SWP_NOZORDER | SWP_SHOWWINDOW);
    const auto editor = viewer.window(), input = terminal.window(), background = backgroundTerminal.window();
    const auto find = FindWindowExW(editor, nullptr, L"PoppyFindBar", nullptr);
    check(find != nullptr, "Cannot find the editor's search input");
    MoveWindow(editor, 0, 0, 640, 300, FALSE);
    MoveWindow(input, 0, 300, 640, 200, FALSE);
    MoveWindow(background, 0, 500, 640, 200, FALSE);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    // Explicitly share an input context, as Windows does by default. No IME
    // language pack or interactive candidate selection is needed for this test.
    struct ContextOwner {
        HIMC value = ImmCreateContext();
        std::vector<std::pair<HWND, HIMC>> previous;
        ~ContextOwner() {
            for (const auto& entry : previous) ImmAssociateContext(entry.first, entry.second);
            if (value) ImmDestroyContext(value);
        }
    } context;
    check(context.value != nullptr, "Cannot create the shared IME context");
    for (const auto window : {editor, input, background, find, quickOpen.window(), projectFind.window(), renameWindow, navigator.window()})
        context.previous.emplace_back(window, ImmAssociateContext(window, context.value));
    const auto caret = [&](HWND window) {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == window,
            "The focused input lost its native IME caret");
        check(!(info.flags & GUI_CARETBLINKING), "The native IME caret draws over Direct2D");
        return info.rcCaret;
    };
    Renderer metrics;
    const auto checkPosition = [&](HWND window, UINT dpi, Renderer::Font kind = Renderer::Font::Code) {
        const auto rect = caret(window);
        COMPOSITIONFORM composition{}; CANDIDATEFORM candidate{};
        check(ImmGetCompositionWindow(context.value, &composition) && ImmGetCandidateWindow(context.value, 0, &candidate),
            "Cannot inspect the IME window positions");
        check(composition.dwStyle == CFS_POINT && composition.ptCurrentPos.x == rect.left && composition.ptCurrentPos.y == rect.top,
            "The IME composition is detached from the insertion point");
        check(candidate.dwStyle == CFS_EXCLUDE && candidate.ptCurrentPos.x == rect.left && candidate.ptCurrentPos.y == rect.bottom,
            "The IME candidate position is detached from the insertion point");
        check(candidate.rcArea.top == rect.top && candidate.rcArea.bottom == rect.bottom,
            "IME candidates can overlap the input row");
        auto excluded = candidate.rcArea;
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&excluded), 2);
        POINT screenPoint{rect.left, rect.top}; ClientToScreen(window, &screenPoint);
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        check(GetMonitorInfoW(MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST), &monitor) &&
            excluded.left <= monitor.rcWork.left && excluded.right >= monitor.rcWork.right,
            "IME candidates can move alongside the input instead of above or below it");
        LOGFONTW font{};
        check(ImmGetCompositionFontW(context.value, &font) != FALSE, "Cannot inspect the IME composition font");
        metrics.setDpi(dpi);
        const auto& expected = metrics.compositionFont(kind);
        check(std::wstring(font.lfFaceName) == expected.lfFaceName && font.lfHeight == expected.lfHeight &&
            font.lfWidth == expected.lfWidth && font.lfWeight == expected.lfWeight && font.lfItalic == expected.lfItalic,
            "The IME composition font does not match the focused input at this DPI");
    };
    const auto resetPosition = [&] {
        COMPOSITIONFORM composition{}; composition.dwStyle = CFS_POINT;
        CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS;
        check(ImmSetCompositionWindow(context.value, &composition) && ImmSetCandidateWindow(context.value, &candidate),
            "Cannot simulate an IME position reset");
        LOGFONTW font{}; font.lfHeight = -8; lstrcpyW(font.lfFaceName, L"Arial");
        check(ImmSetCompositionFontW(context.value, &font) != FALSE, "Cannot simulate an IME font reset");
    };
    const auto refresh = [&](HWND window, UINT dpi, Renderer::Font kind = Renderer::Font::Code) {
        const MSG events[] = {
            {window, WM_IME_SETCONTEXT, TRUE, 0, 0, {0, 0}},
            {window, WM_INPUTLANGCHANGE, 0, reinterpret_cast<LPARAM>(GetKeyboardLayout(0)), 0, {0, 0}},
            {window, WM_IME_STARTCOMPOSITION, 0, 0, 0, {0, 0}},
            {window, WM_IME_COMPOSITION, 0, GCS_COMPSTR, 0, {0, 0}},
            {window, WM_IME_NOTIFY, IMN_OPENCANDIDATE, 1, 0, {0, 0}},
            {window, WM_IME_NOTIFY, IMN_CHANGECANDIDATE, 1, 0, {0, 0}}
        };
        for (const auto& event : events) {
            resetPosition();
            SendMessageW(window, event.message, event.wParam, event.lParam);
            checkPosition(window, dpi, kind);
        }
        SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0);
    };
    auto buffer = std::make_shared<EditBuffer>();
    buffer->document.text = "first\n\t" + std::string(400, 'a') + u8"日本語\nlast";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"ime.txt");
    for (const UINT dpi : {96u, 120u, 144u, 192u}) for (const bool wrap : {true, false}) {
        const UINT otherDpi = dpi == 96 ? 144 : 96;
        SetFocus(editor); viewer.setDpi(dpi); viewer.setWordWrap(wrap);
        viewer.restore({0, 0, 8, 8});
        SendMessageW(editor, WM_KEYDOWN, VK_END, 0);
        checkPosition(editor, dpi);
        for (int i = 0; i < 8; ++i) {
            terminal.setDpi(dpi); backgroundTerminal.setDpi(otherDpi);
            SendMessageW(input, WM_VSCROLL, SB_BOTTOM, 0);
            SendMessageW(background, WM_VSCROLL, SB_BOTTOM, 0);
            SendMessageW(find, WM_SIZE, 0, 0);
            SendMessageW(quickOpen.window(), WM_SIZE, 0, 0);
            Terminal::pump();
            checkPosition(editor, dpi);
        }
        refresh(editor, dpi);
        SendMessageW(editor, WM_CHAR, '!', 0); checkPosition(editor, dpi);
        SetFocus(input); checkPosition(input, dpi); refresh(input, dpi);
        const auto before = caret(input);
        viewer.setDpi(dpi); backgroundTerminal.setDpi(otherDpi);
        SendMessageW(quickOpen.window(), WM_SIZE, 0, 0);
        SendMessageW(background, WM_VSCROLL, SB_BOTTOM, 0);
        checkPosition(input, dpi);
        terminal.setDpi(otherDpi); checkPosition(input, otherDpi);
        check(caret(input).bottom - caret(input).top != before.bottom - before.top,
            "A terminal DPI change did not resize its native IME caret");
        SetFocus(background); checkPosition(background, otherDpi);
        SetFocus(editor); checkPosition(editor, dpi);
    }
    for (const UINT dpi : {96u, 120u, 144u, 192u}) {
        navigator.clear();
        const auto treeRoot = navigator.insert(0, L"alpha", true, 0);
        navigator.insert(treeRoot, L"xray", false, 0); navigator.insert(treeRoot, L"xeno", false, 0); navigator.expand(treeRoot, true);
        viewer.setDpi(dpi); viewer.find();
        projectFind.setDpi(dpi); rename.setDpi(dpi); quickOpen.setDpi(dpi); navigator.setDpi(dpi);
        for (const auto window : {find, projectFind.window(), renameWindow, quickOpen.window(), navigator.window()}) {
            if (window == quickOpen.window()) quickOpen.show(); else SetFocus(window);
            checkPosition(window, dpi, Renderer::Font::Ui);
            refresh(window, dpi, Renderer::Font::Ui);
            // A shared HIMC must retain the UI font while other inputs resize.
            terminal.setDpi(dpi == 96 ? 192 : 96); backgroundTerminal.setDpi(120);
            SendMessageW(editor, WM_SIZE, 0, 0);
            checkPosition(window, dpi, Renderer::Font::Ui);
            const auto query = projectFind.query(), name = rename.text();
            const auto treeSelection = navigator.selection(); const auto activations = treeActivations;
            SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
            const auto before = caret(window);
            for (const auto key : {VK_LEFT, VK_RETURN, VK_ESCAPE, VK_BACK, VK_DELETE}) SendMessageW(window, WM_KEYDOWN, key, 0);
            SendMessageW(window, WM_CHAR, L'x', 0);
            const auto during = caret(window);
            check(IsWindow(window) && GetFocus() == window && EqualRect(&before, &during) &&
                projectFind.query() == query && rename.text() == name && navigator.selection() == treeSelection && treeActivations == activations,
                "Preedit keys changed or closed a proportional input before commit");
            SendMessageW(window, WM_IME_COMPOSITION, 0, 0);
            SendMessageW(window, WM_CHAR, L'x', 0);
            if (window == projectFind.window()) check(projectFind.query() != query, "Cancelled search composition blocked ordinary input");
            if (window == navigator.window()) check(navigator.selection() != treeSelection, "Cancelling type-to-select composition blocked ordinary input");
            if (window == renameWindow) check(rename.text() != name, "Cancelled rename composition blocked ordinary input");
            SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0);
            SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
            SetFocus(parent);
            if (window == quickOpen.window()) quickOpen.show(); else SetFocus(window);
            const auto afterFocus = projectFind.query(), afterName = rename.text();
            SendMessageW(window, WM_CHAR, L'y', 0);
            if (window == projectFind.window()) check(projectFind.query() != afterFocus, "Focus loss left the search composition active");
            if (window == renameWindow) check(rename.text() != afterName, "Focus loss left the rename composition active");
        }
    }
    // Moving between monitor corners must preserve the horizontal exclusion,
    // including monitors with negative desktop coordinates.
    std::vector<RECT> workAreas;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{}; info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcWork);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workAreas));
    SetFocus(editor); viewer.setDpi(96);
    const auto insertion = caret(editor);
    for (const auto& work : workAreas) {
        const POINT positions[] = {{work.left, work.top}, {work.right - 1, work.bottom - (insertion.bottom - insertion.top)}};
        for (const auto& point : positions) {
            SetWindowPos(parent, nullptr, point.x - insertion.left, point.y - insertion.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(editor, WM_IME_NOTIFY, IMN_CHANGECANDIDATE, 1);
            checkPosition(editor, 96);
        }
    }
    SetWindowPos(parent, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetFocus(editor);
    const auto beforeText = buffer->document.text;
    const auto beforeState = viewer.state();
    SendMessageW(editor, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(editor, WM_KEYDOWN, VK_LEFT, 0); SendMessageW(editor, WM_CHAR, L'x', 0);
    check(buffer->document.text == beforeText && viewer.state().caret == beforeState.caret,
        "Preedit keys changed the document or its insertion point before IME commit");
    SendMessageW(editor, WM_IME_ENDCOMPOSITION, 0, 0);
    SendMessageW(editor, WM_CHAR, L'x', 0);
    check(buffer->document.text.size() == beforeText.size() + 1, "Ending IME composition did not restore ordinary input");
    SendMessageW(editor, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(editor, WM_IME_COMPOSITION, 0, 0);
    SendMessageW(editor, WM_CHAR, L'x', 0);
    check(buffer->document.text.size() == beforeText.size() + 2, "Cancelling IME composition left ordinary input blocked");
    SendMessageW(editor, WM_IME_ENDCOMPOSITION, 0, 0);
    SetFocus(parent);
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == nullptr,
        "Leaving an input retained its native IME caret");
}
int terminalImeFixture(const char* eventName) {
    Handle next(OpenEventW(SYNCHRONIZE, FALSE, wide(eventName).c_str()));
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!next || !GetConsoleMode(output, &mode) || !SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return 1;
    if (!SetConsoleOutputCP(CP_UTF8) || !SetConsoleCP(CP_UTF8)) return 1;
    const char* frames[] = {
        "\x1b[2J\x1b[Hheader\x1b[3;1H> input\x1b[6;1Hstatus\x1b[3;4H\x1b[?25h",
        "\x1b[?25l\x1b[Hworking",
        "\x1b[6;1Hfooter\x1b[?25h",
        "\x1b[3;4H",
        "\x1b[6;2H"
    };
    for (size_t i = 0; i < std::size(frames); ++i) {
        if (i && WaitForSingleObject(next.value, 10000) != WAIT_OBJECT_0) return 1;
        std::cout << frames[i] << "\x1b]0;Poppy IME frame " << i << '\a' << std::flush;
    }
    if (WaitForSingleObject(next.value, 10000) != WAIT_OBJECT_0) return 1;
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetConsoleMode(input, &mode) || !SetConsoleMode(input, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT))) return 1;
    std::cout << "\x1b[2J\x1b[3;1H> input\x1b[3;4H\x1b]0;Poppy IME frame 5\a" << std::flush;
    char character = 0; DWORD count = 0;
    if (!ReadFile(input, &character, 1, &count, nullptr) || count != 1 || character != 'X') return 1;
    // Deliberately hold echo until the UI has begun the next composition.
    if (WaitForSingleObject(next.value, 10000) != WAIT_OBJECT_0) return 1;
    std::cout << "\x1b[3;4H\x1b[1@X\x1b]0;Poppy IME frame 6\a" << std::flush;
    if (WaitForSingleObject(next.value, 10000) != WAIT_OBJECT_0) return 1;
    std::cout << "\x1b[6;1Hfooter\x1b]0;Poppy IME frame 7\a" << std::flush;
    const auto softwareFrame = [](const std::string& text, int frame) {
        const std::string border(32, '-');
        std::cout << "\x1b[?25l\x1b[2J\x1b[2;1H" << border << "\x1b[3;1H\x1b[36m>\x1b[39m " << text <<
            "\x1b[0m\x1b[4;1H" << border << "\x1b[6;1Hfooter\x1b]0;Poppy IME frame " << frame << '\a' << std::flush;
    };
    if (WaitForSingleObject(next.value, 10000) != WAIT_OBJECT_0) return 1;
    softwareFrame("\x1b[7mT\x1b[27mry editing a file", 8);
    if (WaitForSingleObject(next.value, 10000) != WAIT_OBJECT_0) return 1;
    softwareFrame("ああああ\x1b[7mあ\x1b[27mああ", 9);
    if (!ReadFile(input, &character, 1, &count, nullptr) || count != 1 || character != 'Y') return 1;
    if (WaitForSingleObject(next.value, 10000) != WAIT_OBJECT_0) return 1;
    softwareFrame("ああああY\x1b[7mあ\x1b[27mああ", 10);
    if (WaitForSingleObject(next.value, 10000) != WAIT_OBJECT_0) return 1;
    std::cout << "\x1b[Hworking\x1b[6;1Hbusy\x1b]0;Poppy IME frame 11\a" << std::flush;
    return WaitForSingleObject(next.value, 10000) == WAIT_OBJECT_0 ? 0 : 1;
}
void terminalImeOutput() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Terminal IME output test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the terminal IME output window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    const auto eventName = L"Local\\PoppyImeOutput-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Handle next(CreateEventW(nullptr, FALSE, FALSE, eventName.c_str()));
    check(static_cast<bool>(next), "Cannot create the terminal IME output gate");
    std::wstring executable(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    check(count && count < executable.size(), "Cannot locate the terminal IME fixture"); executable.resize(count);
    Terminal terminal; terminal.create(parent, instance); terminal.setDpi(96);
    const auto window = terminal.window(); MoveWindow(window, 0, 0, 640, 360, FALSE); ShowWindow(parent, SW_SHOWNOACTIVATE);
    struct ContextOwner {
        HWND window; HIMC value = ImmCreateContext(), previous = nullptr;
        ~ContextOwner() { ImmAssociateContext(window, previous); if (value) ImmDestroyContext(value); }
    } context{window};
    check(context.value != nullptr, "Cannot create the terminal IME context");
    context.previous = ImmAssociateContext(window, context.value);
    terminal.setDirectory(fs::temp_directory_path()); terminal.setCommand({executable, L"--terminal-ime-fixture", eventName});
    check(terminal.launch(), "Cannot launch the terminal IME fixture");
    const auto waitFrame = [&](int frame) {
        const auto start = GetTickCount64();
        while (terminal.title() != "Poppy IME frame " + std::to_string(frame)) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            Terminal::pump();
            check(GetTickCount64() - start < 10000, "The terminal IME fixture did not produce its frame");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    };
    const auto caret = [&] {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        check(GetFocus() == window && GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == window,
            "Terminal output moved keyboard focus or destroyed the IME caret");
        CANDIDATEFORM candidate{};
        check(ImmGetCandidateWindow(context.value, 0, &candidate) && candidate.ptCurrentPos.x == info.rcCaret.left &&
            candidate.ptCurrentPos.y == info.rcCaret.bottom, "Terminal output detached candidates from the IME caret");
        return info.rcCaret;
    };
    waitFrame(0); SetFocus(window); const auto origin = caret();
    SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
    for (int frame : {1, 2}) {
        SetEvent(next.value); waitFrame(frame);
        SendMessageW(window, WM_IME_NOTIFY, IMN_CHANGECANDIDATE, 1);
        const auto current = caret();
        check(EqualRect(&origin, &current), "A top/footer redraw moved active terminal composition or its candidates");
    }
    SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0); const auto footer = caret();
    check(footer.top != origin.top, "Ending terminal composition did not release its insertion point");
    SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
    SetEvent(next.value); waitFrame(3); const auto held = caret();
    check(EqualRect(&footer, &held), "The next composition did not hold its own insertion point");
    SendMessageW(window, WM_IME_COMPOSITION, 0, 0); const auto cancelled = caret();
    check(EqualRect(&origin, &cancelled), "Cancelling terminal composition did not resume cursor tracking");
    SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0); SetFocus(parent);
    SetEvent(next.value); waitFrame(4); SetFocus(window); const auto refocused = caret();
    check(refocused.top == footer.top && refocused.left != footer.left,
        "Focus loss retained an obsolete terminal composition origin");
    SetEvent(next.value); waitFrame(5); const auto beforeEcho = caret();
    // Ordinary text and IME results share Terminal::send's echo tracking.
    SendMessageW(window, WM_CHAR, 'X', 0);
    SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
    const auto pending = caret();
    check(EqualRect(&beforeEcho, &pending), "Pending input captured an unrelated terminal cursor");
    SetEvent(next.value); waitFrame(6); const auto afterEcho = caret();
    check(afterEcho.left > beforeEcho.left && afterEcho.top == beforeEcho.top,
        "A fast next composition pinned its native caret and candidates before the accepted input was echoed");
    SetEvent(next.value); waitFrame(7); const auto stable = caret();
    check(EqualRect(&afterEcho, &stable), "Redrawing output moved the next composition after its input echo");
    SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0);
    SetEvent(next.value); waitFrame(8);
    Renderer metrics; metrics.setDpi(96);
    const int cw = metrics.cellWidth();
    const auto software = caret();
    check(software.left == 6 + 2 * cw && software.top == origin.top,
        "Claude-style inverse cursor placed the native caret or candidates on the footer");
    SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(window, WM_IME_NOTIFY, IMN_CHANGECANDIDATE, 1);
    const auto softwareHeld = caret();
    check(EqualRect(&software, &softwareHeld), "Starting composition lost the software cursor");
    SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0);
    SetEvent(next.value); waitFrame(9);
    const auto fifth = caret();
    check(fifth.left == 6 + 10 * cw && fifth.top == origin.top,
        "The native IME caret started after the highlighted fifth wide character");
    SendMessageW(window, WM_CHAR, 'Y', 0);
    SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
    const auto softwarePending = caret();
    check(EqualRect(&fifth, &softwarePending), "Pending software-cursor input lost the insertion point");
    SetEvent(next.value); waitFrame(10);
    const auto softwareEcho = caret();
    check(softwareEcho.left == fifth.left + cw && softwareEcho.top == fifth.top,
        "Software-cursor echo did not advance the next composition and candidates");
    SetEvent(next.value); waitFrame(11);
    const auto softwareStable = caret();
    check(EqualRect(&softwareEcho, &softwareStable), "Footer output moved composition from the software cursor");
    SendMessageW(window, WM_IME_ENDCOMPOSITION, 0, 0);
    SetEvent(next.value); terminal.stop();
}
void editorWordSelection() {
    Viewer viewer;
    const auto instance = GetModuleHandleW(nullptr);
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Editor word selection test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 700, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Could not create the word selection test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, instance);
    const HWND window = viewer.window(); MoveWindow(window, 0, 0, 700, 360, FALSE);
    check((GetClassLongPtrW(window, GCL_STYLE) & CS_DBLCLKS) != 0, "The editor does not receive native double-click messages");
    auto buffer = std::make_shared<EditBuffer>();
    buffer->document.text = u8"foo_bar42  += bar.baz\r\n日本語　café_42、😀 next\r\n\r\n";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    Renderer metrics;
    int width = 0, height = 0, gutter = 0;
    const auto point = [&](int column, int row, int quarter = 3) {
        return MAKELPARAM(gutter + column * width + width * quarter / 4, row * height + height / 2);
    };
    const auto doubleClick = [&](LPARAM location) {
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, location);
        SendMessageW(window, WM_LBUTTONUP, 0, location);
        SendMessageW(window, WM_LBUTTONDBLCLK, MK_LBUTTON, location);
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, location);
        SendMessageW(window, WM_LBUTTONUP, 0, location);
        check(GetCapture() != window, "A double-click retained mouse capture");
    };
    const auto selection = [&](size_t begin, std::string_view text) {
        const auto state = viewer.state();
        check(state.anchor == begin && state.caret == begin + text.size(), "A double-click selected the wrong word or collapsed on release");
    };
    for (const bool readOnly : {false, true}) for (const bool wrap : {false, true}) for (const UINT dpi : {96u, 144u, 192u}) {
        viewer.setDpi(dpi); viewer.setWordWrap(wrap); metrics.setDpi(dpi);
        width = metrics.cellWidth(); height = metrics.textHeight() + MulDiv(3, dpi, 96); gutter = MulDiv(70, dpi, 96);
        if (readOnly) viewer.show(std::make_shared<Document>(buffer->document), L"words.txt");
        else viewer.edit(buffer, L"words.txt");
        for (const int column : {0, 4, 8}) { doubleClick(point(column, 0)); selection(0, "foo_bar42"); }
        doubleClick(point(10, 0)); selection(9, "  ");
        doubleClick(point(12, 0)); selection(11, "+=");
        doubleClick(point(16, 0)); selection(14, "bar");
        doubleClick(point(17, 0)); selection(17, ".");
        doubleClick(point(20, 0)); selection(18, "baz");
        doubleClick(point(5, 1)); selection(23, u8"日本語");
        doubleClick(point(7, 1)); selection(32, u8"　");
        doubleClick(point(14, 1)); selection(35, u8"café_42");
        doubleClick(point(18, 1)); selection(43, u8"、😀");
        doubleClick(point(23, 1)); selection(51, "next");
        doubleClick(point(25, 1)); selection(55, "");
        doubleClick(point(2, 2)); selection(57, "");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(2, 0));
        SendMessageW(window, WM_LBUTTONUP, 0, point(2, 0)); selection(3, "");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(1, 0, 1));
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, point(5, 0, 1));
        SendMessageW(window, WM_LBUTTONUP, 0, point(5, 0, 1)); selection(1, "oo_b");
    }
    check(!buffer->dirty(), "Word selection modified the document");
    // A word crossing visual rows is selected in full, using logical lines.
    viewer.setDpi(96); metrics.setDpi(96); viewer.setWordWrap(true);
    width = metrics.cellWidth(); height = metrics.textHeight() + 3; gutter = 70;
    auto longWord = std::make_shared<EditBuffer>();
    longWord->document.text = std::string(2000, 'x') + " tail";
    indexDocument(longWord->document); longWord->initialize(longWord->document.text);
    viewer.edit(longWord, L"wrapped-word.txt");
    doubleClick(point(3, 1)); selection(0, std::string(2000, 'x'));
    const int columns = (Scrollbars::contentRect(window).right - 4 - gutter - 2) / width;
    doubleClick(MAKELPARAM(gutter + columns * width, height / 2)); selection(static_cast<size_t>(columns), "");
    doubleClick(point(3, 1));
    SendMessageW(window, WM_CHAR, 'z', 0);
    check(longWord->document.text == "z tail", "Typing did not replace the double-clicked word");
    viewer.setWordWrap(false);
    longWord->document.text = std::string(200, 'x') + " tail";
    indexDocument(longWord->document); longWord->initialize(longWord->document.text);
    viewer.edit(longWord, L"scrolled-word.txt");
    viewer.restore({0, 100, 0, 0});
    doubleClick(point(3, 0)); selection(0, std::string(200, 'x'));
    auto empty = std::make_shared<EditBuffer>();
    indexDocument(empty->document); empty->initialize(""); viewer.edit(empty, L"empty.txt");
    doubleClick(point(0, 0)); selection(0, "");
    viewer.message(L"Loading"); doubleClick(point(0, 0)); selection(0, "");
    auto diff = std::make_shared<DiffDocument>();
    diff->patch.text = "@@ -1 +1 @@\n-old_name.\n+new_name.\n"; indexDiff(*diff);
    for (const bool inlineView : {false, true}) for (const bool after : {false, true}) {
        viewer.setDiffLayout(inlineView); viewer.showDiff(diff, L"words.diff");
        const int origin = inlineView ? 140 : 70 + (after ? (Scrollbars::contentRect(window).right - 4) / 2 + 1 : 0);
        const int y = (inlineView && after ? height : 0) + height / 2;
        doubleClick(MAKELPARAM(origin + 7 * width + width * 3 / 4, y));
        const std::string word = after ? "new_name" : "old_name";
        selection(diff->patch.text.find(word), word);
        check(viewer.state().after == after, "A double-click selected the wrong diff side");
    }
}
void editorColumnSelection() {
    Viewer viewer;
    const auto instance = GetModuleHandleW(nullptr);
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Editor column selection test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 700, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Could not create the column selection test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, instance);
    const HWND window = viewer.window(); MoveWindow(window, 0, 0, 700, 360, FALSE);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    struct KeyboardState {
        BYTE original[256]{};
        KeyboardState() { GetKeyboardState(original); }
        ~KeyboardState() { SetKeyboardState(original); }
    } keyboard;
    const auto key = [&](WPARAM code, bool ctrl = false, bool shift = false) {
        BYTE keys[256]{}; keys[VK_CONTROL] = ctrl ? 0x80 : 0; keys[VK_SHIFT] = shift ? 0x80 : 0;
        check(SetKeyboardState(keys) != FALSE, "Could not set column selection modifiers");
        SendMessageW(window, WM_KEYDOWN, code, 0);
    };
    const auto input = [&](wchar_t c) {
        BYTE keys[256]{}; SetKeyboardState(keys); SendMessageW(window, WM_CHAR, c, 0);
    };
    auto buffer = std::make_shared<EditBuffer>();
    const std::string original = "abcd\r\n12\r\n\r\nwxyz";
    const auto load = [&] {
        buffer->document.text = original; indexDocument(buffer->document); buffer->initialize(original);
        viewer.edit(buffer, L"columns.txt"); SetFocus(window);
    };
    Renderer metrics;
    int cell = 0, height = 0, gutter = 0;
    const auto point = [&](int column, int row) { return MAKELPARAM(gutter + column * cell, row * height + height / 2); };
    const auto drag = [&](int firstColumn, int firstRow, int lastColumn, int lastRow) {
        SendMessageW(window, WM_MBUTTONDOWN, MK_MBUTTON, point(firstColumn, firstRow));
        check(GetCapture() == window, "Middle dragging did not capture the editor");
        SendMessageW(window, WM_LBUTTONUP, MK_MBUTTON, point(firstColumn, firstRow));
        check(GetCapture() == window, "An unrelated button release ended column selection");
        SendMessageW(window, WM_MOUSEMOVE, MK_MBUTTON, point(lastColumn, lastRow));
        SendMessageW(window, WM_MBUTTONUP, 0, point(lastColumn, lastRow));
        check(GetCapture() != window, "Middle dragging retained mouse capture");
    };
    for (const UINT dpi : {96u, 144u, 192u}) for (const bool wrap : {false, true}) for (const bool reverse : {false, true}) {
        viewer.setDpi(dpi); viewer.setWordWrap(wrap); metrics.setDpi(dpi);
        cell = metrics.cellWidth(); height = metrics.textHeight() + MulDiv(3, dpi, 96); gutter = MulDiv(70, dpi, 96);
        for (const bool readOnly : {false, true}) {
            load();
            if (readOnly) viewer.show(std::make_shared<Document>(buffer->document), L"readonly.txt");
            if (reverse) drag(3, 3, 1, 0); else drag(1, 0, 3, 3);
            check(!buffer->dirty(), "Column selection changed the document");
            key('C', true);
            check(clipboardText(window) == L"bc\r\n2 \r\n  \r\nxy", "Column copying included unselected text or lost empty rows");
            input(L'Q');
            if (readOnly) {
                key('C', true); check(clipboardText(window) == L"bc\r\n2 \r\n  \r\nxy", "Read-only columns were edited");
                continue;
            }
            check(buffer->document.text == "aQd\r\n1Q\r\n Q\r\nwQz", "Typing did not replace every selected column");
            input(L'R'); key(VK_BACK);
            check(buffer->document.text == "aQd\r\n1Q\r\n Q\r\nwQz", "Repeated typing or Backspace lost the multiple cursors");
            key('Z', true); key('Z', true); key('Z', true);
            check(buffer->document.text == original && !buffer->dirty(), "Column edits did not undo as complete operations");
            drag(1, 0, 3, 3); copyText(window, L"A\r\nB\r\nC\r\nD"); key('V', true);
            check(buffer->document.text == "aAd\r\n1B\r\n C\r\nwDz", "Column paste did not distribute clipboard lines");
            key('Z', true); drag(1, 0, 3, 3); key('X', true);
            check(buffer->document.text == "ad\r\n1\r\n\r\nwz", "Column cut joined lines or removed unselected text");
            key('Z', true); drag(2, 0, 2, 3); input(L'Z');
            check(buffer->document.text == "abZcd\r\n12Z\r\n  Z\r\nwxZyz", "Zero-width dragging did not create a cursor per row");
            key('Z', true); drag(1, 0, 3, 3); key(VK_ESCAPE); input(L'!');
            check(std::count(buffer->document.text.begin(), buffer->document.text.end(), '!') == 1, "Escape left multiple cursors active");
            load(); drag(1, 0, 3, 3);
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(1, 0));
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, point(3, 0));
            SendMessageW(window, WM_LBUTTONUP, 0, point(3, 0)); input(L'Z');
            check(buffer->document.text == "aZd\r\n12\r\n\r\nwxyz", "A normal left drag retained column selection");
        }
        const std::string indented = "        a\r\n        b\r\n        c";
        buffer->document.text = indented; indexDocument(buffer->document); buffer->initialize(indented);
        viewer.edit(buffer, L"indented.txt"); SetFocus(window);
        if (reverse) drag(9, 2, 8, 0); else drag(8, 0, 9, 2);
        key(VK_TAB, false, true);
        check(buffer->document.text == "    a\r\n    b\r\n    c", "Shift+Tab did not outdent each column-selected line");
        key('C', true); check(clipboardText(window) == L"a\r\nb\r\nc", "Outdenting lost the column selection");
        key(VK_TAB, false, true);
        check(buffer->document.text == "a\r\nb\r\nc", "Repeated Shift+Tab lost selected lines");
        key('Z', true); check(buffer->document.text == "    a\r\n    b\r\n    c", "Column outdent did not undo as one edit");
        key('Z', true); check(buffer->document.text == indented && !buffer->dirty(), "Undo did not restore the column indents");
    }
    viewer.setDpi(96); viewer.setWordWrap(true); metrics.setDpi(96);
    cell = metrics.cellWidth(); height = metrics.textHeight() + 3; gutter = 70;
    const int columns = (Scrollbars::contentRect(window).right - 4 - gutter - 2) / cell;
    buffer->document.text = std::string(columns * 3, 'x'); indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"wrapped.txt"); drag(1, 0, 3, 2); input(L'Z');
    check(std::count(buffer->document.text.begin(), buffer->document.text.end(), 'Z') == 3 && buffer->document.lines.size() == 1,
        "Column selection did not follow wrapped display rows");
    buffer->document.text = "        " + std::string(columns * 3, 'x'); indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"wrapped.txt"); drag(1, 0, 3, 2); key(VK_TAB, false, true);
    check(buffer->document.text == "    " + std::string(columns * 3, 'x'), "Shift+Tab outdented a wrapped logical line more than once");
    auto before = std::make_shared<Document>(); before->text = "abcd\r\n12\r\n\r\nold!"; indexDocument(*before);
    for (const bool inlineView : {false, true}) {
        load(); viewer.setDiffLayout(inlineView); viewer.editDiff(buffer, before, L"columns.txt");
        gutter = inlineView ? 140 : Scrollbars::contentRect(window).right / 2 + 1 + 70;
        drag(1, 0, 3, inlineView ? 4 : 3); key('C', true);
        check(clipboardText(window) == L"bc\r\n2 \r\n  \r\nxy", "Diff column copying included patch prefixes or removed rows");
        input(L'Q'); check(buffer->document.text == "aQd\r\n1Q\r\n Q\r\nwQz", "Diff column edits used patch offsets");
        key('Z', true); check(buffer->document.text == original, "Diff column Undo changed unrelated text");
        input(L'Z'); check(std::count(buffer->document.text.begin(), buffer->document.text.end(), 'Z') == 1, "Undo left stale diff column cursors");
        auto indentedBefore = std::make_shared<Document>(); indentedBefore->text = "    a\r\n    old"; indexDocument(*indentedBefore);
        const std::string indented = "    a\r\n    b";
        buffer->document.text = indented; indexDocument(buffer->document); buffer->initialize(indented);
        viewer.editDiff(buffer, indentedBefore, L"indented.txt"); drag(4, 0, 5, inlineView ? 2 : 1);
        key(VK_TAB, false, true);
        check(buffer->document.text == "a\r\nb", "Diff column outdent changed patch metadata or skipped working lines");
        key('C', true); check(clipboardText(window) == L"a\r\nb", "Diff column outdent lost its mapped selections");
        key('Z', true); check(buffer->document.text == indented && !buffer->dirty(), "Diff column outdent did not undo together");
        gutter = 70;
    }
    viewer.setWordWrap(false); gutter = 70;
    buffer->document.text.clear(); for (int row = 0; row < 100; ++row) buffer->document.text += "abcdef\n";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text); viewer.edit(buffer, L"scroll.txt");
    SendMessageW(window, WM_MBUTTONDOWN, MK_MBUTTON, point(1, 1));
    const auto bottom = MAKELPARAM(gutter + 3 * cell, 500);
    SendMessageW(window, WM_MOUSEMOVE, MK_MBUTTON, bottom);
    for (int i = 0; i < 5; ++i) SendMessageW(window, WM_TIMER, 0x5058, 0);
    check(GetScrollPos(window, SB_VERT) > 0, "Middle dragging below the editor did not auto-scroll");
    SendMessageW(window, WM_MBUTTONUP, 0, bottom); const int top = GetScrollPos(window, SB_VERT);
    for (int i = 0; i < 5; ++i) SendMessageW(window, WM_TIMER, 0x5058, 0);
    check(GetCapture() != window && GetScrollPos(window, SB_VERT) == top, "Released middle dragging kept scrolling");
    viewer.message(L"Loading"); input(L'Z'); check(!buffer->dirty(), "Changing content left stale column cursors");
}
void editorWordMovement() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Editor word movement test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the word movement test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr));
    const HWND window = viewer.window(); MoveWindow(window, 0, 0, 640, 360, FALSE);
    struct KeyboardState {
        BYTE original[256]{};
        KeyboardState() { check(GetKeyboardState(original) != FALSE, "Could not save keyboard state"); }
        ~KeyboardState() { SetKeyboardState(original); }
    } keyboard;
    const auto key = [&](WPARAM code, bool ctrl, bool shift) {
        BYTE keys[256]{};
        keys[VK_CONTROL] = ctrl ? 0x80 : 0; keys[VK_SHIFT] = shift ? 0x80 : 0;
        check(SetKeyboardState(keys) != FALSE, "Could not set test modifiers");
        SendMessageW(window, WM_KEYDOWN, code, 0);
    };
    const auto selection = [&](size_t anchor, size_t caret) {
        const auto state = viewer.state();
        check(state.anchor == anchor && state.caret == caret, "Word movement lost the caret or selection anchor");
    };
    auto buffer = std::make_shared<EditBuffer>();
    buffer->document.text = "foo_bar42  += bar.baz\r\n\r\n  end \t";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    for (const bool wrap : {false, true}) for (const UINT dpi : {96u, 144u}) {
        viewer.setWordWrap(wrap); viewer.setDpi(dpi); viewer.edit(buffer, L"words.txt"); SetFocus(window);
        const std::vector<size_t> stops{0, 11, 14, 17, 18, 27, 32};
        for (size_t i = 1; i < stops.size(); ++i) { key(VK_RIGHT, true, false); selection(stops[i], stops[i]); }
        key(VK_RIGHT, true, false); selection(32, 32);
        for (size_t i = stops.size() - 1; i > 0; --i) { key(VK_LEFT, true, false); selection(stops[i - 1], stops[i - 1]); }
        key(VK_LEFT, true, false); selection(0, 0);
        viewer.restore({0, 0, 14, 14});
        key(VK_RIGHT, true, true); selection(14, 17);
        key(VK_RIGHT, true, true); selection(14, 18);
        key(VK_RIGHT, true, true); selection(14, 27);
        key(VK_LEFT, true, true); selection(14, 18);
        key(VK_LEFT, true, true); selection(14, 17);
        key(VK_LEFT, true, true); selection(14, 14);
        key(VK_LEFT, true, true); selection(14, 11);
        key(VK_LEFT, true, false); selection(11, 11);
        viewer.restore({0, 0, 27, 14});
        key(VK_LEFT, true, true); selection(27, 11);
        key(VK_RIGHT, true, false); selection(27, 27);
        key(VK_LEFT, false, false); selection(26, 26);
        key(VK_RIGHT, false, true); selection(26, 27);
        viewer.restore({0, 0, 0, 0}); key(VK_LEFT, true, true); selection(0, 0);
        viewer.restore({0, 0, 32, 32}); key(VK_RIGHT, true, true); selection(32, 32);
    }
    check(!buffer->dirty(), "Word movement modified the document");
    buffer->document.text = u8"日本語　café_42、😀 next\r\n最後";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"unicode-words.txt");
    for (const size_t offset : {12u, 20u, 28u, 34u, 40u}) { key(VK_RIGHT, true, true); selection(0, offset); }
    for (const size_t offset : {34u, 28u, 20u, 12u, 0u}) { key(VK_LEFT, true, true); selection(0, offset); }
    viewer.restore({0, 0, 23, 23}); key(VK_RIGHT, false, false); selection(27, 27);
    viewer.restore({0, 0, 32, 32}); key(VK_RIGHT, false, false); selection(34, 34);

    buffer->document.text = std::string(2000, 'x') + "  tail";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"wrapped-words.txt");
    key(VK_RIGHT, true, true); selection(0, 2002);
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == window &&
        info.rcCaret.top >= 0 && info.rcCaret.bottom <= Scrollbars::contentRect(window).bottom && viewer.state().wrapOffset > 0,
        "A word jump across wrapped rows did not reveal the caret");
    key(VK_LEFT, true, true); selection(0, 0);
    check(!buffer->dirty(), "Word selection modified the document");
}
void editorWordDeletion() {
    Viewer viewer;
    const auto parent = CreateWindowExW(0, L"STATIC", L"Editor word deletion test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the word deletion test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr));
    const auto window = viewer.window(); MoveWindow(window, 0, 0, 640, 360, FALSE);
    BYTE original[256]{}; GetKeyboardState(original);
    struct KeyboardOwner { BYTE* value; ~KeyboardOwner() { SetKeyboardState(value); } } keyboard{original};
    const auto key = [&](WPARAM code, bool ctrl = true) {
        BYTE keys[256]{}; keys[VK_CONTROL] = ctrl ? 0x80 : 0;
        SetKeyboardState(keys); SendMessageW(window, WM_KEYDOWN, code, 0);
        // TranslateMessage can leave this queued after Ctrl has been released.
        SetKeyboardState(original);
        if (code == VK_BACK) SendMessageW(window, WM_CHAR, ctrl ? 0x7f : '\b', 0);
    };
    struct Case { std::string left, removed, right; };
    const std::vector<Case> cases{
        {"hello ", "world", ""}, {"hello ", "wor", "ld"},
        {"", "foo_bar42", ".tail"}, {"foo", "+=", "bar"},
        {"one ", "two \t", "tail"}, {"", "one\r\n  ", "two"},
        {"", "one\n\n  ", "two"}, {"", " \t\r\n", ""},
        {u8"日本語　", u8"café_42", u8"、😀"}, {u8"日本語", u8"、😀", " tail"},
        {"", u8"日本語", " tail"}, {"", "", "tail"}, {"", "", ""},
        {"", std::string(2000, 'x'), " tail"}
    };
    for (const bool wrap : {false, true}) for (const auto& test : cases) {
        auto buffer = std::make_shared<EditBuffer>();
        const auto text = test.left + test.removed + test.right;
        buffer->document.text = text; indexDocument(buffer->document); buffer->initialize(text);
        viewer.setWordWrap(wrap); viewer.edit(buffer, L"words.txt"); SetFocus(window);
        const size_t offset = test.left.size() + test.removed.size();
        viewer.restore({0, 0, offset, offset}); key(VK_BACK);
        check(buffer->document.text == test.left + test.right, "Ctrl+Backspace deleted the wrong word or inserted DEL");
        check(viewer.state().anchor == test.left.size() && viewer.state().caret == test.left.size(), "Word deletion left the wrong caret");
        if (test.removed.empty()) { check(!buffer->dirty() && buffer->revision == 0, "Word deletion at the start modified the document"); continue; }
        key('Z'); check(!buffer->dirty() && buffer->document.text == text, "Undo did not restore the whole deleted word");
        key('Y'); check(buffer->document.text == test.left + test.right, "Redo did not repeat word deletion");
    }
    for (const bool reversed : {false, true}) {
        auto buffer = std::make_shared<EditBuffer>(); buffer->document.text = "hello world";
        indexDocument(buffer->document); buffer->initialize(buffer->document.text); viewer.edit(buffer, L"selection.txt");
        viewer.restore({0, 0, reversed ? 9u : 7u, reversed ? 7u : 9u}); key(VK_BACK);
        check(buffer->document.text == "hello wld", "Ctrl+Backspace extended a selection to the word boundary");
        key('Z'); viewer.restore({0, 0, 11, 11}); key(VK_BACK, false);
        check(buffer->document.text == "hello worl", "Plain Backspace stopped deleting one character");
    }
}
void editorLineShortcuts() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Editor line shortcuts test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the line shortcuts test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr));
    const HWND window = viewer.window(); MoveWindow(window, 0, 0, 640, 360, FALSE);
    struct KeyboardState {
        BYTE original[256]{};
        KeyboardState() { check(GetKeyboardState(original) != FALSE, "Could not save keyboard state"); }
        ~KeyboardState() { SetKeyboardState(original); }
    } keyboard;
    const auto key = [&](UINT message, WPARAM code, bool ctrl, bool shift, bool alt) {
        BYTE keys[256]{};
        keys[VK_CONTROL] = ctrl ? 0x80 : 0; keys[VK_SHIFT] = shift ? 0x80 : 0; keys[VK_MENU] = alt ? 0x80 : 0;
        check(SetKeyboardState(keys) != FALSE, "Could not set line shortcut modifiers");
        SendMessageW(window, message, code, alt ? 1LL << 29 : 0);
    };
    const auto selection = [&](size_t anchor, size_t caret) {
        check(viewer.state().anchor == anchor && viewer.state().caret == caret, "Line shortcut lost the caret or selection direction");
    };
    auto buffer = std::make_shared<EditBuffer>();
    const auto load = [&](const std::string& text, size_t anchor, size_t caret) {
        buffer->document.text = text; indexDocument(buffer->document); buffer->initialize(text);
        viewer.edit(buffer, L"lines.txt"); viewer.restore({0, 0, anchor, caret}); SetFocus(window);
    };
    const std::string original = u8"first\r\n日本😀\r\nlast";
    for (const bool wrap : {false, true}) for (const UINT dpi : {96u, 144u}) {
        viewer.setWordWrap(wrap); viewer.setDpi(dpi);
        load(original, 10, 10);
        key(WM_KEYDOWN, 'X', true, false, false);
        check(buffer->document.text == "first\r\nlast" && clipboardText(window) == L"日本😀\r\n", "Ctrl+X did not cut the entire logical line");
        selection(7, 7);
        key(WM_KEYDOWN, 'Z', true, false, false);
        check(buffer->document.text == original && !buffer->dirty(), "Undo did not restore the cut line");
        key(WM_KEYDOWN, 'Y', true, false, false);
        check(buffer->document.text == "first\r\nlast", "Redo did not repeat the line cut");

        load(original, 10, 10);
        key(WM_SYSKEYDOWN, VK_UP, false, true, true);
        check(buffer->document.text == u8"日本😀\r\nfirst\r\nlast", "Shift+Alt+Up did not swap lines through WM_SYSKEYDOWN");
        selection(3, 3);
        key(WM_SYSKEYDOWN, VK_DOWN, false, true, true);
        check(buffer->document.text == original, "Shift+Alt+Down did not move the line back"); selection(10, 10);
        key(WM_KEYDOWN, VK_DOWN, false, true, true);
        check(buffer->document.text == u8"first\r\nlast\r\n日本😀", "Shift+Alt+Down did not handle WM_KEYDOWN"); selection(16, 16);
        key(WM_KEYDOWN, 'Z', true, false, false);
        check(buffer->document.text == original, "Undo did not restore swapped lines");

        load("a\r\nbb\r\nccc\r\ndddd", 12, 4);
        key(WM_SYSKEYDOWN, VK_UP, false, true, true);
        check(buffer->document.text == "bb\r\nccc\r\na\r\ndddd", "Line movement included the line at the exclusive selection end"); selection(9, 1);
        key(WM_SYSKEYDOWN, VK_DOWN, false, true, true);
        check(buffer->document.text == "a\r\nbb\r\nccc\r\ndddd", "Selected lines did not move back as one block"); selection(12, 4);
        key(WM_KEYDOWN, 'X', true, false, false);
        check(buffer->document.text == "a\r\nbdddd" && clipboardText(window) == L"b\r\nccc\r\n", "Ctrl+X changed selected-text cutting");

        load("only", 2, 2); key(WM_SYSKEYDOWN, VK_UP, false, true, true); selection(2, 2);
        key(WM_SYSKEYDOWN, VK_DOWN, false, true, true);
        check(!buffer->dirty() && buffer->revision == 0, "A line move at the document boundary created an edit");
        viewer.show(std::make_shared<Document>(buffer->document), L"readonly.txt");
        check(copyText(window, L"unchanged"), "Could not prepare the read-only clipboard test");
        key(WM_KEYDOWN, 'X', true, false, false); key(WM_SYSKEYDOWN, VK_DOWN, false, true, true);
        check(!viewer.buffer() && buffer->document.text == "only" && clipboardText(window) == L"unchanged", "Line shortcuts edited a read-only document");

        load("    one", 6, 6); key(WM_KEYDOWN, VK_TAB, false, true, false);
        check(buffer->document.text == "one", "Shift+Tab inserted a tab instead of outdenting"); selection(2, 2);
        key(WM_KEYDOWN, VK_TAB, false, false, false);
        check(buffer->document.text == "on\te", "Plain Tab no longer inserts a tab"); selection(3, 3);
        const std::string indented = "    a\r\n\tb\r\n  c";
        load(indented, 11, 0); key(WM_KEYDOWN, VK_TAB, false, true, false);
        check(buffer->document.text == "a\r\nb\r\n  c", "Shift+Tab included the line at the exclusive selection end"); selection(6, 0);
        const auto revision = buffer->revision;
        key(WM_KEYDOWN, VK_TAB, false, true, false);
        check(buffer->revision == revision, "An outdent without indentation created an edit"); selection(6, 0);
        key(WM_KEYDOWN, 'Z', true, false, false);
        check(buffer->document.text == indented && !buffer->dirty(), "Selected-line outdent did not undo in one operation");
        key(WM_KEYDOWN, 'Y', true, false, false);
        check(buffer->document.text == "a\r\nb\r\n  c", "Redo did not restore the outdented lines");
        load("    readonly", 4, 4);
        viewer.show(std::make_shared<Document>(buffer->document), L"readonly.txt");
        key(WM_KEYDOWN, VK_TAB, false, true, false);
        check(buffer->document.text == "    readonly" && !buffer->dirty(), "Shift+Tab edited a read-only document");
    }
    viewer.setWordWrap(true);
    load(std::string(2000, 'x') + "\nlast", 1000, 1000);
    key(WM_SYSKEYDOWN, VK_DOWN, false, true, true);
    check(buffer->document.text == "last\n" + std::string(2000, 'x'), "Line movement used a wrapped screen row"); selection(1005, 1005);
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == window &&
        info.rcCaret.top >= 0 && info.rcCaret.bottom <= Scrollbars::contentRect(window).bottom,
        "Moving a wrapped line did not reveal the caret");
    key(WM_KEYDOWN, 'X', true, false, false);
    check(buffer->document.text == "last" && clipboardText(window) == std::wstring(2000, L'x'), "Ctrl+X cut a wrapped screen row or left the last line behind");
}
void editorWrapping() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Editor wrapping test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the wrapping test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr)); viewer.setDpi(96);
    const HWND window = viewer.window(); MoveWindow(window, 0, 0, 640, 360, FALSE);
    auto buffer = std::make_shared<EditBuffer>();
    buffer->document.text = std::string(2000, 'x');
    buffer->document.text.replace(1500, 6, "needle");
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"wrapped.txt"); SetFocus(window);
    Renderer metrics; metrics.setDpi(96);
    const int cell = metrics.cellWidth(), height = metrics.textHeight() + 3;
    const auto caret = [&] {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == window, "Wrapped text lost the IME caret");
        return info.rcCaret;
    };
    const int columns = (Scrollbars::contentRect(window).right - 4 - 70 - 2) / cell;
    const auto original = buffer->document.text;
    check(viewer.wordWrap() && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL), "Wrapping is not enabled by default or left a horizontal scrollbar");
    SendMessageW(window, WM_KEYDOWN, VK_END, 0);
    check(viewer.state().caret == static_cast<size_t>(columns) && caret().top == 0, "End did not stop at the end of the first wrapped row");
    SendMessageW(window, WM_KEYDOWN, VK_DOWN, 0);
    check(viewer.state().caret == static_cast<size_t>(columns * 2) && caret().top == height, "Down did not preserve the trailing column on the next wrapped row");
    SendMessageW(window, WM_KEYDOWN, VK_UP, 0);
    check(viewer.state().caret == static_cast<size_t>(columns) && caret().top == 0, "Up skipped a wrapped row");
    SendMessageW(window, WM_KEYDOWN, VK_RIGHT, 0);
    check(viewer.state().caret == static_cast<size_t>(columns + 1) && caret().left == 70 + cell && caret().top == height, "Right did not cross a wrapping boundary");
    SendMessageW(window, WM_KEYDOWN, VK_HOME, 0);
    check(viewer.state().caret == static_cast<size_t>(columns) && caret().left == 70, "Home did not move to the start of a continuation");
    const auto point = [&](int column, int row) { return MAKELPARAM(70 + column * cell, row * height + height / 2); };
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(3, 2));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, point(5, 3));
    SendMessageW(window, WM_LBUTTONUP, 0, point(5, 3));
    check(viewer.state().anchor == static_cast<size_t>(columns * 2 + 3) && viewer.state().caret == static_cast<size_t>(columns * 3 + 5), "Mouse selection did not follow wrapped rows");
    SendMessageW(window, WM_CHAR, '!', 0);
    check(buffer->document.text.size() == original.size() - columns - 1 && buffer->document.text.find('\n') == std::string::npos, "Editing wrapped text inserted a newline or replaced the wrong bytes");
    size_t anchor = 0, insertion = 0;
    check(buffer->undo(anchor, insertion) && buffer->document.text == original, "Undo did not restore the wrapped selection");
    viewer.edit(buffer, L"wrapped.txt");
    check(viewer.selectSearch(1500, 1506, "needle", true), "Could not navigate to a match deep in a wrapped logical line");
    check(viewer.state().wrapOffset > 0 && caret().top >= 0 && caret().bottom <= Scrollbars::contentRect(window).bottom, "A match in a continuation was not revealed");
    const auto saved = viewer.state();
    MoveWindow(window, 0, 0, 360, 360, FALSE);
    TextLayout layout; layout.build(buffer->document, (Scrollbars::contentRect(window).right - 4 - 70 - 2) / cell);
    const auto topRow = layout.row(buffer->document, layout.rowForOffset(buffer->document, saved.wrapOffset));
    check(viewer.state().wrapOffset == topRow.begin && viewer.state().caret == saved.caret, "Resizing lost the top source position or selection");
    viewer.restore(saved);
    check(viewer.state().wrapOffset == topRow.begin, "Restoring a wrapped tab used the old viewport width");
    viewer.setWordWrap(false);
    check(!viewer.wordWrap() && (GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL), "Disabling wrapping did not restore horizontal scrolling");
    viewer.setWordWrap(true);
    check(!(GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL) && viewer.state().caret == saved.caret && buffer->document.text == original && !buffer->dirty(), "Toggling wrapping changed the buffer or selection");
    viewer.setDpi(144); SendMessageW(window, WM_PAINT, 0, 0);
    check(!(GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL), "DPI reflow restored horizontal scrolling");
    viewer.show(std::make_shared<Document>(buffer->document), L"read-only.txt");
    check(viewer.selectSearch(1500, 1506, "needle", true) && viewer.state().wrapOffset > 0, "Read-only text did not wrap or reveal a continuation match");
}
void diffWrapping() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Diff wrapping test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 700, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the diff wrapping window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr)); viewer.setDpi(96);
    const HWND window = viewer.window(); MoveWindow(window, 0, 0, 700, 360, FALSE);
    Renderer metrics; metrics.setDpi(96);
    const int cell = metrics.cellWidth(), height = metrics.textHeight() + 3;
    auto diff = std::make_shared<DiffDocument>();
    diff->patch.text = "diff --git a/file b/file\n@@ -1,32 +1,32 @@\n-" + std::string(2000, 'r') + "\n+short\n";
    for (int i = 0; i < 31; ++i) diff->patch.text += " context\n";
    indexDiff(*diff, true); viewer.showDiff(diff, L"wrapped.diff");
    check(viewer.textStatus().empty(), "Diff status incorrectly describes the patch as the source file encoding");
    const auto original = diff->patch.text;
    const size_t before = diff->offset(diff->rows[1].before), after = diff->offset(diff->rows[1].after);
    const auto info = [&] { SCROLLINFO value{sizeof(value), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window, SB_VERT, &value); return value; };
    const int columns = (Scrollbars::contentRect(window).right - 4 - 140 - 2) / cell;
    check(viewer.wordWrap() && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL), "Inline diffs did not inherit word wrapping");
    const auto point = [&](int column, int row) { return MAKELPARAM(140 + column * cell, row * height + height / 2); };
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(3, 2));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, point(5, 3));
    SendMessageW(window, WM_LBUTTONUP, 0, point(5, 3));
    check(viewer.state().anchor == before + columns + 3 && viewer.state().caret == before + columns * 2 + 5,
        "Inline diff selection did not map wrapped rows to source bytes");
    check(diff->selectedText(false, viewer.state().anchor, viewer.state().caret, true) == std::string(columns + 2, 'r'),
        "Copying a diff continuation inserted a soft-wrap newline");
    for (int i = 0; i < 10; ++i) SendMessageW(window, WM_VSCROLL, SB_LINEDOWN, 0);
    const auto saved = viewer.state(); const int top = info().nPos;
    check(saved.top == 1 && saved.wrapOffset > 0, "Inline scrolling did not retain a source continuation");
    viewer.showDiff(diff, L"wrapped.diff", true);
    check(viewer.state().wrapOffset == saved.wrapOffset && info().nPos == top, "Refreshing a diff lost its wrapped position");
    viewer.setDiffLayout(false);
    DiffLayout layout;
    layout.build(*diff, false, ((Scrollbars::contentRect(window).right - 4 - 1) / 2 - 70 - 2) / cell);
    const auto expected = layout.textRow(*diff, layout.rowForOffset(*diff, 1, saved.wrapOffset, false), false);
    check(viewer.state().wrapOffset == expected.begin && viewer.state().anchor == saved.anchor && viewer.state().caret == saved.caret,
        "Changing diff layouts lost the top source position or selection");
    // The top source row extends beyond the right side. Clicking that padding
    // maps to the right line's end while the viewport retains the longer side.
    const int right = (Scrollbars::contentRect(window).right - 4) / 2 + 1;
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(right + 70, height / 2));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(right + 70, height / 2));
    check(viewer.state().anchor == saved.anchor && viewer.state().caret == saved.caret && GetCapture() != window,
        "Clicking the sticky filename must not select the hidden source row");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(right + 70, height + height / 2));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(right + 70, height + height / 2));
    const auto padded = viewer.state(); const int splitTop = info().nPos;
    check(padded.after && padded.caret == after + 5 && padded.wrapOffset > 0, "Split padding did not map to the shorter side's end");
    viewer.restore({}); viewer.restore(padded);
    check(info().nPos == splitTop && viewer.state().wrapOffset == padded.wrapOffset && viewer.state().after,
        "Restoring split padding used the selected shorter side's row count");
    MoveWindow(window, 0, 0, 420, 360, FALSE);
    layout.build(*diff, false, ((Scrollbars::contentRect(window).right - 4 - 1) / 2 - 70 - 2) / cell);
    const auto resized = layout.textRow(*diff, layout.rowForOffset(*diff, 1, padded.wrapOffset, false), false);
    check(viewer.state().wrapOffset == resized.begin && viewer.state().caret == padded.caret, "Resizing a split diff lost its source position");
    viewer.setDiffLayout(true); SendMessageW(window, WM_PAINT, 0, 0);
    check(viewer.state().top == 1 && viewer.state().wrapOffset > 0, "Inline layout restored the shorter selected side instead of the top source");
    for (bool inlineView : {false, true}) {
        viewer.setDiffLayout(inlineView); viewer.setWordWrap(false);
        check(GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL, "Disabling wrapping did not restore diff horizontal scrolling");
        viewer.setWordWrap(true); SendMessageW(window, WM_PAINT, 0, 0);
        check(!(GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL), "Enabling wrapping left a diff horizontal scrollbar");
    }
    for (const UINT dpi : {96u, 144u, 192u}) {
        viewer.setDpi(dpi); metrics.setDpi(dpi);
        MoveWindow(window, 0, 0, MulDiv(420, dpi, 96), MulDiv(360, dpi, 96), FALSE);
        for (const bool withFind : {false, true}) {
            if (withFind) {
                viewer.find();
                const auto input = FindWindowExW(window, nullptr, L"PoppyFindBar", nullptr);
                RECT bounds{}; GetWindowRect(input, &bounds); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
                check(bounds.top == 0, "The removed Before/After header still occupies space above diff search");
            }
            const int header = MulDiv(34, dpi, 96), rowHeight = metrics.textHeight() + MulDiv(3, dpi, 96);
            for (const bool inlineView : {false, true}) {
                viewer.setDiffLayout(inlineView);
                const int rightEdge = Scrollbars::contentRect(window).right - header / 3;
                const auto selection = viewer.state();
                // The filename now uses the whole row; the old button area
                // must neither toggle the layout nor start a selection.
                for (const int x : {rightEdge - header, rightEdge - 3 * header}) {
                    const auto point = MAKELPARAM(x, (withFind ? header : 0) + rowHeight / 2);
                    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    SendMessageW(window, WM_LBUTTONUP, 0, point);
                }
                check(viewer.diffInline() == inlineView && GetCapture() != window && viewer.state().anchor == selection.anchor && viewer.state().caret == selection.caret,
                    "Clicking the filename retained old layout buttons or changed selection after scrolling, resizing, DPI or search");
            }
            if (withFind) SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        }
        SendMessageW(window, WM_PAINT, 0, 0);
    }
    check(diff->patch.text == original && viewer.state().caret == padded.caret, "Diff wrapping changed the patch or selection");
    viewer.showDiff(std::make_shared<DiffDocument>(), L"empty.diff"); SendMessageW(window, WM_PAINT, 0, 0);
    check(info().nMax == 0, "An empty diff retained wrapped rows");
}
void documentFind() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Document search test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the search test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr)); viewer.setDpi(96);
    const HWND window = viewer.window(); MoveWindow(window, 0, 0, 640, 360, FALSE);
    auto buffer = std::make_shared<EditBuffer>();
    buffer->document.text = u8"needle NEEDLE\n日本語🙂\nneedle";
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"search.txt"); viewer.find();
    const HWND input = FindWindowExW(window, nullptr, L"PoppyFindBar", nullptr);
    check(input != nullptr && GetFocus() == input, "Ctrl+F did not focus its search input");
    const auto status = [&] { wchar_t title[256]{}; GetWindowTextW(input, title, 256); return std::wstring(title); };
    const auto wait = [&] {
        const auto start = GetTickCount64();
        while (status() == L"Find in file: Searching…") {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            check(GetTickCount64() - start < 5000, "Document search did not finish");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    };
    const auto query = [&](const std::wstring& text) {
        viewer.find();
        for (const auto ch : text) SendMessageW(input, WM_CHAR, ch, 0);
        wait();
    };
    query(L"needle");
    check(viewer.state().anchor == 0 && viewer.state().caret == 6 && status() == L"Find in file: 1 / 3", "Initial document match or count failed");
    SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0); wait();
    check(viewer.state().anchor == 7 && viewer.state().caret == 13, "Enter did not select the next match");
    SendMessageW(input, WM_KEYDOWN, VK_UP, 0); wait();
    check(viewer.state().anchor == 0, "Previous match did not navigate backwards");
    SendMessageW(input, WM_KEYDOWN, VK_UP, 0); wait();
    check(viewer.state().caret == buffer->document.text.size(), "Previous match did not wrap");
    query(L"日本語🙂");
    check(buffer->document.text.substr(viewer.state().anchor, viewer.state().caret - viewer.state().anchor) == u8"日本語🙂", "IME/surrogate UTF-16 input lost a UTF-8 match");
    query(L"needle");
    SetFocus(window); SendMessageW(window, WM_KEYDOWN, VK_END, 0); SendMessageW(window, WM_CHAR, '!', 0);
    const auto edited = viewer.state(); wait();
    check(buffer->dirty() && viewer.state().anchor == edited.anchor && viewer.state().caret == edited.caret,
        "Recounting after an edit moved the insertion point");
    // A document switch must discard the old scan and preserve the new tab's selection.
    viewer.find(); SendMessageW(input, WM_CHAR, 'n', 0);
    auto replacement = std::make_shared<Document>(); replacement->text = "new needle"; indexDocument(*replacement);
    viewer.show(replacement, L"replacement.txt"); wait();
    check(viewer.state().anchor == 0 && viewer.state().caret == 0 && status() == L"Find in file: 2 matches", "A stale search selected text in a new document");
    query(L"needle");
    check(viewer.state().anchor == 4 && viewer.state().caret == 10, "Read-only document search failed");
    check(viewer.selectSearch(4, 10, "NEEDLE", false) && !viewer.selectSearch(4, 10, "NEEDLE", true) && !viewer.selectSearch(4, 11, "needle", false),
        "Project search navigation failed to validate stale offsets or case options");
    auto diff = std::make_shared<DiffDocument>();
    diff->patch.text = "diff --git a/needle b/needle\n--- a/needle\n+++ b/needle\n@@ -1 +1 @@\n-old needle\n+new needle\n";
    indexDiff(*diff); viewer.showDiff(diff, L"diff.txt"); wait();
    query(L"needle");
    check(status() == L"Find in file: 1 / 2" && diff->patch.text.substr(viewer.state().anchor - 4, 4) == "old ",
        "Diff search included hidden patch headers");
    viewer.setDiffLayout(false); SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0); wait();
    check(viewer.state().after && diff->patch.text.substr(viewer.state().anchor - 4, 4) == "new ", "Split diff search selected the wrong side");
    SendMessageW(input, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!(GetWindowLongPtrW(input, GWL_STYLE) & WS_VISIBLE) && GetFocus() == window, "Escape did not close search and restore editor focus");
    SendMessageW(window, WM_KEYDOWN, VK_F3, 0); wait();
    check(!viewer.state().after, "F3 with a closed search bar did not advance and wrap");
    auto longDiff = std::make_shared<DiffDocument>();
    longDiff->patch.text = "@@ -1 +1 @@\n-" + std::string(2000, 'x') + "needle\n+" + std::string(1000, 'y') + "needle\n";
    indexDiff(*longDiff);
    for (bool inlineView : {true, false}) {
        viewer.setDiffLayout(inlineView); viewer.showDiff(longDiff, L"long.diff"); wait();
        query(L"needle");
        check(status() == L"Find in file: 1 / 2" && !viewer.state().after, "Wrapped diff search selected the wrong removal");
        Renderer metrics; metrics.setDpi(96); DiffLayout layout;
        const int contentWidth = Scrollbars::contentRect(window).right - 4;
        const int width = inlineView ? contentWidth : (contentWidth - 1) / 2;
        layout.build(*longDiff, inlineView, (width - (inlineView ? 140 : 70) - 2) / metrics.cellWidth());
        const auto visible = [&] {
            const auto match = longDiff->inlineRowForText(viewer.state().anchor, viewer.state().caret);
            check(match.has_value(), "Wrapped search selected hidden diff metadata");
            const auto change = longDiff->displayRow(*match, true); const auto source = viewer.state().after ? change.after : change.before;
            const auto row = layout.rowForOffset(*longDiff, longDiff->rowForLine(source, viewer.state().after, inlineView), viewer.state().anchor - longDiff->offset(source), viewer.state().after);
            SCROLLINFO info{sizeof(info), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window, SB_VERT, &info);
            check(row >= static_cast<size_t>(info.nPos) && row < static_cast<size_t>(info.nPos) + info.nPage, "Search did not reveal the matched diff continuation");
        };
        visible(); SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0); wait();
        check(viewer.state().after && status() == L"Find in file: 2 / 2", "Wrapped diff search selected the wrong addition"); visible();
    }
}
struct ScrollbarWindow {
    Scrollbars bars;
    HWND window = nullptr;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
        auto* self = reinterpret_cast<ScrollbarWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<ScrollbarWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        LRESULT result = 0;
        if (self && self->bars.handle(window, message, a, b, result)) return result;
        if (self && (message == WM_VSCROLL || message == WM_HSCROLL)) {
            const int bar = message == WM_VSCROLL ? SB_VERT : SB_HORZ;
            auto info = self->info(window, bar);
            switch (LOWORD(a)) {
            case SB_THUMBTRACK: case SB_THUMBPOSITION: info.nPos = self->bars.trackPosition(bar, info.nTrackPos); break;
            case SB_LINEUP: --info.nPos; break; case SB_LINEDOWN: ++info.nPos; break;
            case SB_PAGEUP: info.nPos -= static_cast<int>(info.nPage); break;
            case SB_PAGEDOWN: info.nPos += static_cast<int>(info.nPage); break;
            }
            info.fMask = SIF_POS; SetScrollInfo(window, bar, &info, FALSE); return 0;
        }
        return DefWindowProcW(window, message, a, b);
    }
    static SCROLLINFO info(HWND window, int bar) {
        SCROLLINFO value{sizeof(value), SIF_ALL, 0, 0, 0, 0, 0};
        check(GetScrollInfo(window, bar, &value) != FALSE, "The themed scrollbar lost its Win32 scroll information");
        return value;
    }
    ScrollbarWindow() {
        WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"PoppyScrollbarTest";
        RegisterClassW(&cls);
        window = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP | WS_VSCROLL | WS_HSCROLL,
            0, 0, 320, 128, nullptr, nullptr, cls.hInstance, this);
        check(window != nullptr, "Could not create the scrollbar test window");
        for (const int bar : {SB_VERT, SB_HORZ}) {
            SCROLLINFO value{sizeof(value), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 999999, 100, 0, 0};
            SetScrollInfo(window, bar, &value, FALSE);
        }
    }
    ~ScrollbarWindow() { DestroyWindow(window); }
};
struct ViewerPaintWatch {
    HWND window;
    WNDPROC original;
    size_t paints = 0;
    POINT painted{};
    explicit ViewerPaintWatch(HWND value) : window(value) {
        check(SetPropW(window, L"PoppyViewerPaintWatch", this) != FALSE, "Could not observe viewer paints");
        original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure)));
        check(original != nullptr, "Could not subclass the viewer");
    }
    ~ViewerPaintWatch() {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        RemovePropW(window, L"PoppyViewerPaintWatch");
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
        auto& self = *static_cast<ViewerPaintWatch*>(GetPropW(window, L"PoppyViewerPaintWatch"));
        const auto result = CallWindowProcW(self.original, window, message, a, b);
        if (message == WM_PAINT) {
            ++self.paints;
            self.painted = {ScrollbarWindow::info(window, SB_HORZ).nPos, ScrollbarWindow::info(window, SB_VERT).nPos};
        }
        return result;
    }
};
void horizontalWheelScrolling() {
    ScrollbarWindow pane;
    const auto wheel = [&](UINT message, int delta, WORD keys = 0) {
        SendMessageW(pane.window, message, MAKEWPARAM(keys, static_cast<WORD>(delta)), 0);
    };
    const auto left = [&] { return ScrollbarWindow::info(pane.window, SB_HORZ).nPos; };
    UINT characters = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &characters, 0);
    const int distance = characters == WHEEL_PAGESCROLL ? 100 : static_cast<int>(std::min(characters, 999900u));
    wheel(WM_MOUSEWHEEL, -WHEEL_DELTA / 2, MK_SHIFT);
    check(left() == 0, "A partial horizontal wheel tick moved before reaching its threshold");
    LRESULT result = 0;
    check(!pane.bars.handle(pane.window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0, result),
        "Horizontal wheel handling consumed an ordinary vertical wheel event");
    wheel(WM_MOUSEWHEEL, -WHEEL_DELTA / 2, MK_SHIFT);
    check(left() == distance && ScrollbarWindow::info(pane.window, SB_VERT).nPos == 0,
        "Shift+wheel did not accumulate partial ticks independently or changed the vertical position");
    wheel(WM_MOUSEWHEEL, WHEEL_DELTA, MK_SHIFT);
    check(left() == 0, "Shift+wheel up did not move left");
    wheel(WM_MOUSEHWHEEL, WHEEL_DELTA);
    check(left() == distance, "A native horizontal wheel tick did not move right");
    wheel(WM_MOUSEHWHEEL, -WHEEL_DELTA);
    check(left() == 0, "A native horizontal wheel tick did not move left");
    wheel(WM_MOUSEWHEEL, WHEEL_DELTA, MK_SHIFT);
    check(left() == 0, "Horizontal wheel input escaped the left boundary");
    SCROLLINFO value{sizeof(value), SIF_POS, 0, 0, 0, 999900, 0};
    SetScrollInfo(pane.window, SB_HORZ, &value, FALSE);
    wheel(WM_MOUSEWHEEL, -WHEEL_DELTA, MK_SHIFT);
    check(left() == 999900, "Horizontal wheel input escaped the right boundary");
    value = {sizeof(value), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 1, 0, 0};
    SetScrollInfo(pane.window, SB_HORZ, &value, FALSE);
    check(pane.bars.handle(pane.window, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, static_cast<WORD>(-WHEEL_DELTA)), 0, result)
        && pane.bars.handle(pane.window, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0, result)
        && ScrollbarWindow::info(pane.window, SB_VERT).nPos == 0,
        "A pane without horizontal overflow did not consume horizontal input");
}
void editorBottomPadding() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Editor bottom padding", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create bottom padding test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    Viewer viewer, popup;
    viewer.create(parent, instance); popup.create(parent, instance, true);
    const auto window = viewer.window();
    const auto info = [&] { return ScrollbarWindow::info(window, SB_VERT); };
    Renderer metrics;
    for (const bool wrap : {false, true}) for (const UINT dpi : {96u, 144u, 192u}) {
        viewer.setWordWrap(wrap); viewer.setDpi(dpi); metrics.setDpi(dpi);
        for (const int height : {180, 360}) {
            MoveWindow(window, 0, 0, 640, height, FALSE);
            for (const auto& text : {std::string(), std::string("one\nlast"), std::string(2000, 'x'), std::string(2000, 'x') + "\nlast\n"}) {
                auto document = std::make_shared<Document>(); document->text = text; indexDocument(*document);
                viewer.show(document, L"padding.txt");
                const auto checkBottom = [&] {
                    TextLayout layout;
                    const int columns = std::max(1, (static_cast<int>(Scrollbars::contentRect(window).right) - MulDiv(4, dpi, 96) - MulDiv(70, dpi, 96) - 2) / metrics.cellWidth());
                    layout.build(*document, columns);
                    const int rows = static_cast<int>(wrap ? layout.rows() : document->lines.size());
                    SendMessageW(window, WM_VSCROLL, SB_BOTTOM, 0);
                    check(info().nPos == rows - 1, "Final visual row cannot reach the editor top");
                    const auto saved = viewer.state();
                    check(saved.top == static_cast<int>(document->lines.size()) - 1, "Bottom padding became a source row");
                    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
                    check(info().nPos == rows - 1, "Wheel scrolled beyond the final source row");
                    viewer.restore({}); viewer.restore(saved);
                    check(info().nPos == rows - 1, "Restoring a view lost the final row at the top");
                    SendMessageW(window, WM_PAINT, 0, 0);
                    check(document->text == text, "Scroll padding modified the document");
                };
                checkBottom();
                viewer.find(); checkBottom();
                SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
            }
        }
        MoveWindow(popup.window(), 0, 0, 640, 360, FALSE); popup.setDpi(dpi);
        auto shortMessage = std::make_shared<Document>(); shortMessage->text = "one\ntwo"; indexDocument(*shortMessage);
        popup.show(shortMessage, {}, false, false);
        check(!(GetWindowLongPtrW(popup.window(), GWL_STYLE) & WS_VSCROLL), "A short message popup acquired editor padding");
        for (const bool inlineView : {false, true}) for (const bool headings : {false, true}) {
            auto diff = std::make_shared<DiffDocument>();
            diff->patch.text = "diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-before\n+after\n";
            indexDocument(diff->patch); indexDiff(*diff, headings);
            viewer.setDiffLayout(inlineView); viewer.showDiff(diff, L"changes.diff");
            DiffLayout layout;
            const int width = (Scrollbars::contentRect(window).right - MulDiv(4, dpi, 96) - (inlineView ? 0 : 1)) / (inlineView ? 1 : 2);
            layout.build(*diff, inlineView, std::max(1, (width - MulDiv(inlineView ? 140 : 70, dpi, 96) - 2) / metrics.cellWidth()));
            const int rows = static_cast<int>(wrap ? layout.rows() : diff->rowCount(inlineView));
            SendMessageW(window, WM_VSCROLL, SB_BOTTOM, 0);
            check(info().nPos == rows - 1 - (headings ? 1 : 0), "Diff padding hid the last row behind the sticky filename");
            SendMessageW(window, WM_PAINT, 0, 0);
        }
        viewer.showDiff(std::make_shared<DiffDocument>(), L"empty.diff");
        check(info().nMax == 0, "An empty comparison acquired scroll padding");
        viewer.message(L"No file");
        check(info().nMax == 0, "A status message retained editor padding");
    }
}
void editorTailFollowing() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Tail following test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Could not create the tail following test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    Viewer viewer, other;
    viewer.create(parent, instance); other.create(parent, instance);
    MoveWindow(viewer.window(), 0, 0, 640, 360, FALSE); MoveWindow(other.window(), 0, 0, 480, 240, FALSE);
    const auto scrollInfo = [](HWND window) {
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 0, 0, 0};
        check(GetScrollInfo(window, SB_VERT, &info) != FALSE, "Cannot read tail viewport");
        return info;
    };
    const auto atBottom = [&](HWND window) {
        const auto info = scrollInfo(window);
        return info.nPos == std::max(0, info.nMax - static_cast<int>(info.nPage) + 1);
    };
    const auto lines = [](int count) { std::string text; while (count--) text += "log entry 日本語\r\n"; return text; };
    for (const bool wrap : {false, true}) for (const UINT dpi : {96u, 144u, 192u}) {
        viewer.setDpi(dpi); viewer.setWordWrap(wrap); other.setDpi(dpi); other.setWordWrap(wrap);
        auto buffer = std::make_shared<EditBuffer>(); buffer->document.text = lines(100) + "partial";
        indexDocument(buffer->document); buffer->initialize(buffer->document.text);
        viewer.edit(buffer, L"output.log"); other.edit(buffer, L"output.log");
        viewer.restore({0, 0, 2, 5});
        const auto reload = [&](std::string text, bool external = true) {
            EditBuffer loaded; loaded.document.text = std::move(text);
            indexDocument(loaded.document); loaded.initialize(loaded.document.text);
            *buffer = std::move(loaded);
            viewer.refreshBuffer(external); other.refreshBuffer(external);
            // The application's load completion also reattaches the shared buffer.
            viewer.edit(buffer, L"output.log", true);
        };
        SendMessageW(viewer.window(), WM_VSCROLL, SB_BOTTOM, 0);
        const int oldTop = scrollInfo(viewer.window()).nPos;
        reload(buffer->document.text + " completed\r\n" + lines(30));
        check(atBottom(viewer.window()) && scrollInfo(viewer.window()).nPos > oldTop, "External append did not follow the tail");
        check(scrollInfo(other.window()).nPos == 0, "A shared buffer forced the other pane to the tail");
        check(viewer.state().anchor == 2 && viewer.state().caret == 5 && !buffer->dirty(), "Tail following moved the selection or dirtied the buffer");
        SendMessageW(viewer.window(), WM_VSCROLL, SB_LINEUP, 0);
        const auto paused = viewer.state();
        reload(buffer->document.text + lines(30));
        check(viewer.state().top == paused.top && viewer.state().wrapOffset == paused.wrapOffset && !atBottom(viewer.window()), "Scrolling up one row did not pause tail following");
        SendMessageW(viewer.window(), WM_VSCROLL, SB_BOTTOM, 0);
        reload(buffer->document.text + lines(30));
        check(atBottom(viewer.window()), "Returning to the bottom did not resume tail following");
        const auto localTop = scrollInfo(viewer.window()).nPos;
        reload(buffer->document.text + lines(30), false);
        check(scrollInfo(viewer.window()).nPos == localTop && !atBottom(viewer.window()), "A local buffer refresh unexpectedly enabled tail following");
        SendMessageW(viewer.window(), WM_VSCROLL, SB_BOTTOM, 0);
        reload("short\r\n");
        check(atBottom(viewer.window()) && scrollInfo(viewer.window()).nPos == 1, "Truncating a tailed file left an invalid viewport");
        reload(lines(100));
        check(atBottom(viewer.window()), "A file growing beyond its first page did not follow the tail");
        reload(std::string(1000, 'x'));
        SendMessageW(viewer.window(), WM_VSCROLL, SB_BOTTOM, 0);
        const auto range = scrollInfo(viewer.window()).nMax;
        reload(buffer->document.text + std::string(3000, 'y'));
        check(atBottom(viewer.window()) && (!wrap || scrollInfo(viewer.window()).nMax > range), "Appending within a wrapped line did not follow visual rows");
        if (!wrap) {
            SendMessageW(viewer.window(), WM_HSCROLL, SB_LINERIGHT, 0);
            const auto left = viewer.state().left;
            reload(buffer->document.text + "\r\n" + lines(100));
            check(atBottom(viewer.window()) && viewer.state().left == left, "Tail following lost horizontal scroll position");
        }
        auto before = std::make_shared<Document>(buffer->document);
        for (const bool inlineView : {false, true}) {
            viewer.setDiffLayout(inlineView); viewer.editDiff(buffer, before, L"output.log");
            SendMessageW(viewer.window(), WM_VSCROLL, SB_BOTTOM, 0);
            const auto diffTop = viewer.state();
            buffer->document.text += "\r\n";
            for (int row = 0; row < 100; ++row) buffer->document.text += "new external diff entry\r\n";
            indexDocument(buffer->document);
            viewer.refreshBuffer(true);
            check(viewer.state().top == diffTop.top && viewer.state().wrapOffset == diffTop.wrapOffset, "An external refresh forced a Git comparison to the tail");
        }
        viewer.edit(buffer, L"another.log");
        check(scrollInfo(viewer.window()).nPos == 0, "Opening another file inherited tail following");
    }
}
void largeFileTailPreview() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Large file tail test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Could not create the large file tail test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    Viewer viewer; viewer.create(parent, instance); MoveWindow(viewer.window(), 0, 0, 640, 360, FALSE);
    const auto snapshot = [](int first, int count) {
        auto document = std::make_shared<Document>(); document->tail = true;
        document->fileBytes = 5ull * 1024 * 1024 * 1024;
        for (int i = first; i < first + count; ++i) document->text += std::to_string(i) + u8" 日本😀\r\n";
        indexDocument(*document); return document;
    };
    const auto atBottom = [&] {
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 0, 0, 0};
        GetScrollInfo(viewer.window(), SB_VERT, &info);
        return info.nPos == std::max(0, info.nMax - static_cast<int>(info.nPage) + 1);
    };
    int requests = 0; viewer.tailRequested = [&] { ++requests; };
    for (const bool wrap : {false, true}) for (const UINT dpi : {96u, 144u, 192u}) {
        viewer.setDpi(dpi); viewer.setWordWrap(wrap);
        auto first = snapshot(0, 100);
        viewer.showTail(first, L"large.log");
        check(viewer.tailPreview() && !viewer.tailPaused() && atBottom() && !viewer.buffer(), "A large file did not open read-only at EOF");
        check(viewer.textStatus().find(L"TAIL") != std::wstring::npos && viewer.textStatus().find(L"READ ONLY") != std::wstring::npos, "Tail status did not explain the partial view");
        SendMessageW(viewer.window(), WM_CHAR, 'x', 0);
        SendMessageW(viewer.window(), WM_KEYDOWN, VK_DELETE, 0);
        check(first->text == snapshot(0, 100)->text && !viewer.buffer(), "Typing changed a partial tail document");
        SendMessageW(viewer.window(), WM_VSCROLL, SB_LINEUP, 0);
        check(viewer.tailPaused(), "Scrolling up did not pause a bounded tail");
        const auto position = viewer.state();
        viewer.showTail(snapshot(30, 100), L"large.log", true);
        viewer.selectSearch(0, 1, "0", true);
        check(viewer.state().caret == 1, "A paused tail replaced the text under its selection");
        viewer.restore(position);
        const auto oldRequests = requests;
        SendMessageW(viewer.window(), WM_VSCROLL, SB_BOTTOM, 0);
        check(requests == oldRequests + 1 && !viewer.tailPaused(), "Returning to EOF did not request the latest snapshot");
        viewer.showTail(snapshot(30, 100), L"large.log", true);
        check(atBottom() && !viewer.tailPaused(), "A sliding tail window lost EOF following");
        viewer.showTail(snapshot(0, 1), L"large.log", true);
        check(atBottom() && !viewer.buffer(), "Log rotation lost EOF or enabled editing");
        viewer.showTail(snapshot(0, 100), L"large.log", true);
        check(atBottom(), "A truncated tail stopped following growth");
        viewer.showTail(snapshot(0, 100), L"large.csv");
        check(!viewer.csvAvailable() && !viewer.csvTable(), "A partial CSV opened as a complete table");
        viewer.showTail(snapshot(0, 100), L"large.html");
        check(!viewer.htmlAvailable() && !viewer.htmlPreview(), "A partial HTML document launched a whole-file browser preview");
        viewer.show(std::make_shared<Document>(), L"ordinary.txt");
        check(!viewer.tailPreview() && viewer.textStatus().find(L"TAIL") == std::wstring::npos, "An ordinary file inherited tail mode");
    }
}
void continuousViewerScrolling() {
    Viewer viewer;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Continuous scrolling test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the scrolling test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, GetModuleHandleW(nullptr)); viewer.setDpi(96);
    viewer.setWordWrap(false);
    const HWND window = viewer.window();
    check(MoveWindow(window, 0, 0, 640, 360, FALSE) != FALSE, "Could not size the scrolling test viewer");
    auto buffer = std::make_shared<EditBuffer>();
    const auto line = std::string(maxLineColumns, 'x') + '\n';
    for (int row = 0; row < 256; ++row) buffer->document.text += line;
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    viewer.edit(buffer, L"long-lines.html");
    viewer.setHtmlPreview(false);
    ShowWindow(parent, SW_SHOWNOACTIVATE); SetFocus(window);
    ViewerPaintWatch observed(window);
    const auto checkPaint = [&](size_t before) {
        check(observed.paints > before, "Continuous input deferred the viewer paint until scrolling stopped");
        check(observed.painted.x == viewer.state().left && observed.painted.y == viewer.state().top,
            "The viewer painted an outdated scroll position");
    };
    for (const int bar : {SB_HORZ, SB_VERT}) {
        viewer.restore({}); UpdateWindow(window);
        RECT client{}; GetClientRect(window, &client);
        const auto content = Scrollbars::contentRect(window);
        const int cross = bar == SB_VERT ? (content.right + client.right) / 2 : (content.bottom + client.bottom) / 2;
        const int arrow = bar == SB_VERT ? client.right - content.right : client.bottom - content.bottom;
        const auto point = [&](int along) { return bar == SB_VERT ? MAKELPARAM(cross, along) : MAKELPARAM(along, cross); };
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point(arrow + 1));
        check(GetCapture() == window, "The viewer did not capture its scrollbar thumb");
        const int length = bar == SB_VERT ? content.bottom : content.right;
        // Keep input queued, with jumps large enough to replace the visible
        // text. Do not dispatch a queued WM_PAINT between mouse moves.
        for (int step = 1; step <= 10; ++step)
            check(PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, point(arrow + 1 + length * step / 10)) != FALSE,
                "Could not queue a continuous thumb drag");
        MSG message{}; size_t moved = 0;
        while (PeekMessageW(&message, window, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) {
            const int before = ScrollbarWindow::info(window, bar).nPos;
            const auto paints = observed.paints; DispatchMessageW(&message);
            if (ScrollbarWindow::info(window, bar).nPos != before) { checkPaint(paints); ++moved; }
        }
        check(moved >= 6, "The drag did not exercise enough distinct large scroll positions");
        SendMessageW(window, WM_LBUTTONUP, 0, point(length));
        check(GetCapture() != window, "Continuous scrolling retained mouse capture");
    }
    viewer.restore({}); UpdateWindow(window);
    for (int step = 0; step < 8; ++step) {
        const auto paints = observed.paints;
        SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        checkPaint(paints);
    }
    viewer.restore({}); UpdateWindow(window);
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, static_cast<WORD>(-WHEEL_DELTA / 2)), 0);
    check(viewer.state().left == 0 && viewer.state().top == 0, "A partial Shift+wheel tick moved the viewer");
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    const auto top = viewer.state().top;
    const auto paints = observed.paints;
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, static_cast<WORD>(-WHEEL_DELTA / 2)), 0);
    UINT characters = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &characters, 0);
    check(viewer.state().top == top, "Shift+wheel changed the viewer's vertical position");
    if (characters) {
        check(viewer.state().left > 0, "Shift+wheel down did not move the viewer right"); checkPaint(paints);
        SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, WHEEL_DELTA), 0);
        check(viewer.state().left == 0 && viewer.state().top == top, "Shift+wheel up did not return the viewer left");
    }
    viewer.setWordWrap(true);
    const auto wrappedTop = viewer.state().top;
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, static_cast<WORD>(-WHEEL_DELTA)), 0);
    check(viewer.state().left == 0 && viewer.state().top == wrappedTop, "Shift+wheel scrolled a wrapped viewer vertically");
}
// Keep the pointer stationary between ticks to catch implementations that only
// scroll on WM_MOUSEMOVE. One real timer also verifies scheduling on mouse down.
constexpr UINT_PTR selectionTimer = 0x5058;
void selectionTicks(HWND window, int count) {
    for (int i = 0; i < count; ++i) SendMessageW(window, WM_TIMER, selectionTimer, 0);
}
void selectionAutoScrolling() {
    Viewer viewer;
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Selection scrolling test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the selection scrolling window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, instance);
    const auto window = viewer.window(); MoveWindow(window, 0, 0, 640, 360, FALSE);
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    auto buffer = std::make_shared<EditBuffer>();
    for (int row = 0; row < 40; ++row) buffer->document.text += u8"日本語😀 " + std::string(160, 'x') + '\n';
    indexDocument(buffer->document); buffer->initialize(buffer->document.text);
    auto diff = std::make_shared<DiffDocument>();
    diff->patch.text = "diff --git a/file.txt b/file.txt\n@@ -1,40 +1,40 @@\n";
    for (int row = 0; row < 40; ++row) diff->patch.text += " " + std::string(160, 'x') + '\n';
    indexDiff(*diff, true);
    bool realTimerChecked = false;
    for (const auto dpi : {96u, 144u}) for (const bool wrap : {false, true}) for (int mode = 0; mode < 4; ++mode) {
        viewer.setDpi(dpi); viewer.setWordWrap(wrap);
        if (mode == 0) viewer.edit(buffer, L"selection.txt");
        else if (mode == 1) viewer.show(std::make_shared<Document>(buffer->document), L"readonly.txt");
        else { viewer.setDiffLayout(mode == 2); viewer.showDiff(diff, L"selection.diff"); }
        SetFocus(window);
        const auto rect = Scrollbars::contentRect(window);
        Renderer metrics; metrics.setDpi(dpi);
        const int height = metrics.textHeight() + MulDiv(3, dpi, 96);
        const int x = MulDiv(mode == 2 ? 140 : 70, dpi, 96) + 3 * metrics.cellWidth();
        const int y = 3 * height + height / 2;
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        check(GetCapture() == window, "Selection did not capture the pointer");
        const auto anchor = viewer.state().anchor;
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, rect.bottom + 2 * height));
        const auto before = viewer.state().caret;
        if (!realTimerChecked) {
            const auto start = GetTickCount64(); bool received = false;
            while (!received) {
                MSG message{};
                while (PeekMessageW(&message, window, WM_TIMER, WM_TIMER, PM_REMOVE)) {
                    received |= message.wParam == selectionTimer;
                    DispatchMessageW(&message);
                }
                check(GetTickCount64() - start < 1000, "Selection scrolling did not start its timer");
                if (!received) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_TIMER);
            }
            realTimerChecked = true;
        }
        selectionTicks(window, 5);
        check(GetScrollPos(window, SB_VERT) > 0 && viewer.state().caret > before && viewer.state().anchor == anchor,
            "Stationary downward dragging did not scroll and extend the same selection");
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
        const int inside = GetScrollPos(window, SB_VERT); selectionTicks(window, 3);
        check(GetScrollPos(window, SB_VERT) == inside, "Returning inside the text did not stop scrolling");
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, -2 * height));
        const auto lower = viewer.state().caret; selectionTicks(window, 2);
        check(GetScrollPos(window, SB_VERT) < inside && viewer.state().caret < lower && viewer.state().anchor == anchor,
            "Upward dragging did not reverse scrolling with a stable anchor");
        selectionTicks(window, 100);
        check(GetScrollPos(window, SB_VERT) == 0, "Dragging above the document did not stop at the beginning");
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, rect.bottom + 1000)); selectionTicks(window, 200);
        const auto end = ScrollbarWindow::info(window, SB_VERT);
        check(end.nPos == end.nMax - static_cast<int>(end.nPage) + 1, "Dragging below the document did not stop at the end");
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, rect.bottom + 1000));
        const auto released = viewer.state(); selectionTicks(window, 3);
        check(GetCapture() != window && viewer.state().caret == released.caret && GetScrollPos(window, SB_VERT) == end.nPos,
            "A stale timer changed a released selection");
    }
    // Cancellation must also kill scrolling without requiring a button-up.
    for (int cancel = 0; cancel < 6; ++cancel) {
        viewer.edit(buffer, L"selection.txt"); SetFocus(window);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(130, 60));
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(130, 500)); selectionTicks(window, 1);
        if (cancel == 0) SendMessageW(window, WM_CANCELMODE, 0, 0);
        if (cancel == 1) { SetCapture(parent); ReleaseCapture(); }
        if (cancel == 2) SetFocus(parent);
        if (cancel == 3) { ShowWindow(window, SW_HIDE); ShowWindow(window, SW_SHOWNA); }
        if (cancel == 4) viewer.message(L"Loading");
        if (cancel == 5) SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        const auto saved = viewer.state(); const int top = GetScrollPos(window, SB_VERT); selectionTicks(window, 3);
        check(GetCapture() != window && GetScrollPos(window, SB_VERT) == top && viewer.state().caret == saved.caret,
            "Cancelling or switching content left selection scrolling active");
    }
    check(!buffer->dirty(), "Selection scrolling edited the buffer");
}
void terminalAsyncStartup() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Poppy", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the asynchronous terminal test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    Handle gate(CreateEventW(nullptr, TRUE, FALSE, nullptr)), entered(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    check(gate && entered, "Cannot create terminal startup gates");
    Terminal terminal; terminal.create(parent, instance); terminal.setDirectory(fs::temp_directory_path());
    MoveWindow(terminal.window(), 0, 0, 640, 360, FALSE);
    const auto uiThread = GetCurrentThreadId();
    std::atomic<unsigned> preparations{0}, failures{0};
    unsigned ready = 0, probes = 0;
    bool invalidCommand = false;
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    const auto command = std::wstring(system) + L"\\cmd.exe";
    struct StopOnExit {
        Terminal& terminal; HANDLE gate;
        ~StopOnExit() { SetEvent(gate); terminal.stop(); terminal.starting = {}; terminal.focused = {}; }
    } stopOnExit{terminal, gate.value};
    terminal.focused = [&] { ++probes; };
    terminal.starting = [&]() -> Terminal::StartupTask {
        check(GetCurrentThreadId() == uiThread, "Startup snapshot did not run on the UI thread");
        return [&, invalidCommand] {
            ++preparations; SetEvent(entered.value);
            check(GetCurrentThreadId() != uiThread, "Blocking terminal preparation ran on the UI thread");
            check(WaitForSingleObject(gate.value, 10000) == WAIT_OBJECT_0, "Terminal startup gate timed out");
            Terminal::LaunchOptions options;
            options.command = {invalidCommand ? command + L".missing" : command, L"/D", L"/Q", L"/K"};
            options.ready = [&] { check(GetCurrentThreadId() == uiThread, "Terminal readiness touched UI from a worker"); ++ready; };
            options.failed = [&] { ++failures; };
            return options;
        };
    };
    const auto dispatch = [&] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Terminal::pump();
    };
    const auto wait = [&](const auto& done) {
        const auto start = GetTickCount64();
        while (!done()) { dispatch(); check(GetTickCount64() - start < 10000, "Asynchronous terminal startup did not complete"); Sleep(10); }
    };
    const auto start = GetTickCount64();
    check(terminal.launch() && GetTickCount64() - start < 1000, "Launching a terminal waited for preparation");
    check(WaitForSingleObject(entered.value, 1000) == WAIT_OBJECT_0, "Terminal preparation did not begin");
    check(terminal.launch() && preparations == 1, "Repeated activation duplicated a pending launch");
    SendMessageW(terminal.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    PostMessageW(terminal.window(), WM_SETFOCUS, 0, 0); dispatch();
    check(probes && terminal.launching() && !ready, "UI messages stopped while terminal startup was pending");
    terminal.setDpi(144); MoveWindow(terminal.window(), 0, 0, 800, 500, FALSE);
    // Preserve input received while the notification relay is still starting.
    for (const auto c : std::wstring(L"title Poppy async ready\r")) SendMessageW(terminal.window(), WM_CHAR, c, 0);
    SetEvent(gate.value);
    wait([&] { return terminal.title() == "Poppy async ready"; });
    check(ready == 1 && preparations == 1 && terminal.running(), "A pending launch lost input or completed more than once");
    wchar_t title[64]{}; GetWindowTextW(parent, title, 64);
    check(std::wstring(title) == L"Poppy", "Terminal startup changed the main window title");
    terminal.stop();

    // A failed child launch cleans up on the worker and permits another try.
    invalidCommand = true; check(terminal.launch(), "Cannot schedule the failing terminal launch");
    wait([&] { return !terminal.launching(); });
    check(!terminal.running() && ready == 1 && failures == 1, "Failed startup retained a process or invoked readiness");
    invalidCommand = false; check(terminal.launch(), "Cannot retry a failed terminal launch");
    wait([&] { return ready == 2; }); terminal.stop();

    // Closing during preparation must join it without applying a stale result.
    ResetEvent(gate.value); ResetEvent(entered.value);
    check(terminal.launch(), "Cannot schedule the cancelled terminal launch");
    check(WaitForSingleObject(entered.value, 1000) == WAIT_OBJECT_0, "Cancelled preparation did not begin");
    NativeThread release([&] { Sleep(50); SetEvent(gate.value); });
    { ShutdownDialog dialog(parent, instance); terminal.stop(&dialog); }
    release.join(); dispatch();
    check(!terminal.launching() && !terminal.running() && ready == 2 && failures == 2,
        "Closing a pending terminal applied its result or left its process running");
    check(terminal.launch(), "Cannot restart a terminal after cancelling preparation");
    wait([&] { return ready == 3; }); terminal.stop();
}
void terminalProcessShutdown() {
    std::wstring module(32768, L'\0'); const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    check(count && count < module.size(), "Cannot locate terminal lifetime fixture"); module.resize(count);
    const auto executable = fs::path(module).parent_path() / L"poppy_process_lifetime_tests.exe";
    const auto directory = fs::temp_directory_path() / (L"Poppy ConPTY lifetime " + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(directory);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{directory};
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Lifetime test", WS_POPUP, 0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create terminal lifetime window");
    struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{parent};
    Terminal first, second;
    int index = 0;
    for (auto terminal : {&first, &second}) {
        terminal->create(parent, instance); terminal->setDirectory(directory);
        terminal->setCommand({executable.native(), L"--tree", (directory / std::to_wstring(index++)).native(), L"Poppy-ConPTY-lifetime"});
        check(terminal->launch(), "Cannot launch terminal lifetime fixture");
    }
    const auto start = GetTickCount64();
    while (first.title() != "Poppy lifetime ready" || second.title() != "Poppy lifetime ready") {
        MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Terminal::pump(); check(GetTickCount64() - start < 10000, "Terminal lifetime fixture did not become ready");
        Sleep(10);
    }
    first.stop();
    Handle lock(CreateFileW((directory / L"0").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(static_cast<bool>(lock), "Terminal shutdown returned while a descendant held its lock");
    check(second.running(), "Closing a terminal stopped its sibling");
    {
        ShutdownDialog dialog(parent, instance);
        dialog.stage(ShutdownDialog::Stage::Stopping, L"Terminal shutdown regression");
        dialog.detail(L"Stopping terminal processes…");
        second.stop(&dialog);
    }
    Handle secondLock(CreateFileW((directory / L"1").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(static_cast<bool>(secondLock), "Modal terminal shutdown returned before its descendants exited");
}
void terminalGitBashSocketEnvironment() {
    const auto bash = terminalShellCommand(TerminalShell::GitBash, L"").front();
    if (!fs::is_regular_file(bash)) {
        std::cout << "Git Bash socket environment test skipped: Git for Windows is not installed\n";
        return;
    }
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Terminal socket environment test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the terminal socket test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    std::wstring powershell = std::wstring(system) + L"/WindowsPowerShell/v1.0/powershell.exe";
    std::replace(powershell.begin(), powershell.end(), L'\\', L'/');
    const std::wstring socket = L"/tmp/poppy-events-regression/events.sock";
    // Read the environment in a native child so MSYS actually applies its
    // conversion rules. No WSL installation or live notification relay is needed.
    for (const std::wstring excluded : {L"", L"POPPY_KEEP_PATH", L"POPPY_KEEP_PATH;", L"*"}) {
        Terminal terminal; terminal.create(parent, instance);
        MoveWindow(terminal.window(), 0, 0, 640, 360, FALSE);
        terminal.setDirectory(fs::temp_directory_path());
        const std::wstring kept = excluded.empty() ? L"kept" : L"/tmp/poppy-keep-path";
        terminal.setEnvironment({{L"AICONTAINER_EVENT_SOCKET", socket}, {L"MSYS2_ENV_CONV_EXCL", excluded}, {L"POPPY_KEEP_PATH", kept}});
        terminal.setCommand({bash, L"--noprofile", L"--norc", L"-c",
            L"\"" + powershell + L"\" -NoLogo -NoProfile -NonInteractive -Command '[Console]::Title = $env:AICONTAINER_EVENT_SOCKET + \"|\" + $env:POPPY_KEEP_PATH'"});
        check(terminal.launch(), "Cannot start the Git Bash socket environment fixture");
        const auto expected = utf8(socket + L"|" + kept);
        const auto start = GetTickCount64();
        while (terminal.running() || terminal.title() != expected) {
            Terminal::pump();
            check(GetTickCount64() - start < 5000, "Git Bash converted the Linux notification socket or lost an existing conversion exclusion");
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        terminal.stop(); DestroyWindow(terminal.window());
    }
}
void terminalSelectionAutoScrolling() {
    Terminal terminal;
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"Terminal selection scrolling test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the terminal scrolling window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    terminal.create(parent, instance); terminal.setDpi(96);
    const auto window = terminal.window(); MoveWindow(window, 0, 0, 640, 360, FALSE); ShowWindow(parent, SW_SHOWNOACTIVATE);
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    terminal.setDirectory(fs::temp_directory_path());
    terminal.setCommand({std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe", L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-Command",
        L"1..120 | ForEach-Object { [Console]::WriteLine(('ROW{0:D3}' -f $_)) }; [Console]::Title = 'Poppy selection fixture ready'"});
    check(terminal.launch(), "Cannot start the terminal scrolling fixture");
    const auto start = GetTickCount64();
    for (;;) {
        Terminal::pump();
        if (!terminal.running() && terminal.title() == "Poppy selection fixture ready" && ScrollbarWindow::info(window, SB_VERT).nPos >= 100) break;
        check(GetTickCount64() - start < 10000, "The terminal scrolling fixture did not produce history");
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    SetFocus(window);
    SendMessageW(window, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(6, 80));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(6, -100));
    const int bottom = GetScrollPos(window, SB_VERT); selectionTicks(window, 5);
    check(GetScrollPos(window, SB_VERT) < bottom, "Terminal drag did not scroll into history");
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(6, 100));
    const int inside = GetScrollPos(window, SB_VERT); selectionTicks(window, 3);
    check(GetScrollPos(window, SB_VERT) == inside, "Terminal kept scrolling after returning inside");
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(6, -100)); selectionTicks(window, 100);
    check(GetScrollPos(window, SB_VERT) == 0, "Terminal drag did not stop at the oldest history");
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(6, -100));
    selectionTicks(window, 3); check(GetScrollPos(window, SB_VERT) == 0, "Released terminal drag kept scrolling");
    SendMessageW(window, WM_COPY, 0, 0);
    const auto upward = clipboardText(window);
    check(upward.find(L"ROW001") != std::wstring::npos && upward.find(L"ROW090") != std::wstring::npos,
        "Upward auto-scrolling lost text across terminal history pages");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(6, 4));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(6, 500)); selectionTicks(window, 100);
    check(GetScrollPos(window, SB_VERT) == bottom, "Terminal drag did not scroll back to the live screen");
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(6, 500)); SendMessageW(window, WM_COPY, 0, 0);
    const auto downward = clipboardText(window);
    check(downward.find(L"ROW001") != std::wstring::npos && downward.find(L"ROW120") != std::wstring::npos,
        "Downward auto-scrolling lost its original terminal anchor");
    for (const auto message : {WM_CANCELMODE, WM_KEYDOWN, WM_KILLFOCUS}) {
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(6, 80));
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(6, -100));
        SendMessageW(window, message, message == WM_KEYDOWN ? VK_ESCAPE : 0, 0);
        const int saved = GetScrollPos(window, SB_VERT); selectionTicks(window, 3);
        check(GetCapture() != window && GetScrollPos(window, SB_VERT) == saved, "Cancelled terminal selection kept scrolling");
    }
}
void themedScrollbars() {
    ScrollbarWindow pane;
    Renderer renderer; renderer.setDpi(96);
    Bitmap bitmap;
    const auto draw = [&] {
        check(renderer.begin(bitmap.bitmap.get()), "Could not render the themed scrollbars");
        renderer.fill(bitmap.bounds(), accent);
        pane.bars.paint(pane.window, renderer, panel);
        finish(renderer); bitmap.read();
    };
    RECT client{}; GetClientRect(pane.window, &client);
    check(client.right == 320 && client.bottom == 128, "Native nonclient scrollbars still consume the custom bars' client area");
    const auto content = Scrollbars::contentRect(pane.window);
    check(content.right < client.right && content.bottom < client.bottom, "The bars overlap the content area");
    draw();
    check(bitmap.pixel(0, 0) == accent, "Scrollbar painting changed the document area");
    check(bitmap.pixel(client.right - 1, client.bottom - 1) == panel, "The scrollbar corner does not match its pane");
    check(bitmap.pixel(content.right, content.bottom / 2) == panel && bitmap.pixel(content.right / 2, content.bottom) == panel,
        "A scrollbar track does not match its pane");
    const auto checkMouseTarget = [&](POINT point) {
        ClientToScreen(pane.window, &point);
        check(SendMessageW(pane.window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) == HTCLIENT,
            "A themed scrollbar routes real mouse input to native nonclient tracking");
    };
    checkMouseTarget({client.right - 1, client.bottom - 1});
    for (const int bar : {SB_VERT, SB_HORZ}) {
        draw();
        const int cross = bar == SB_VERT ? (content.right + client.right) / 2 : (content.bottom + client.bottom) / 2;
        const int length = bar == SB_VERT ? content.bottom : content.right;
        int first = -1, last = -1;
        for (int along = 0; along < length; ++along) {
            if ((bar == SB_VERT ? bitmap.pixel(cross, along) : bitmap.pixel(along, cross)) == scrollbarThumb) {
                if (first < 0) first = along;
                last = along;
            }
        }
        check(first >= 0 && last > first, "A scrollbar thumb is missing its theme color");
        const auto thumb = Scrollbars::thumbRect(pane.window, bar);
        check((bar == SB_VERT ? thumb.top : thumb.left) == first && (bar == SB_VERT ? thumb.bottom : thumb.right) == last + 1,
            "The marker mask uses different thumb boundaries from the painted scrollbar");
        const auto markers = Scrollbars::trackRect(pane.window, bar);
        check((bar == SB_VERT ? markers.top : markers.left) == first,
            "Expanded markers do not start at the scrollbar track");
        const int along = (first + last) / 2;
        const POINT point = bar == SB_VERT ? POINT{cross, along} : POINT{along, cross};
        const LPARAM location = MAKELPARAM(point.x, point.y);
        checkMouseTarget(point);
        SendMessageW(pane.window, WM_MOUSEMOVE, 0, location); draw();
        check(bitmap.pixel(point.x, point.y) == scrollbarHover, "Hovering a thumb did not update its theme color");
        SendMessageW(pane.window, WM_LBUTTONDOWN, MK_LBUTTON, location); draw();
        check(GetCapture() == pane.window && bitmap.pixel(point.x, point.y) == muted, "Dragging a thumb did not capture the pointer or update its color");
        const POINT end = bar == SB_VERT ? POINT{cross, client.bottom + 100} : POINT{client.right + 100, cross};
        SendMessageW(pane.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(end.x, end.y));
        check(ScrollbarWindow::info(pane.window, bar).nPos == 999900, "Dragging truncated a scroll position above 65535 or did not reach the end");
        const auto scrolledMarkers = Scrollbars::trackRect(pane.window, bar);
        check(EqualRect(&markers, &scrolledMarkers), "Scrolling moved the marker track");
        SendMessageW(pane.window, WM_LBUTTONUP, 0, MAKELPARAM(end.x, end.y));
        check(GetCapture() != pane.window, "Releasing the thumb left the pointer captured");
        checkMouseTarget(bar == SB_VERT ? POINT{cross, 2} : POINT{2, cross});
        SendMessageW(pane.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(bar == SB_VERT ? cross : 2, bar == SB_VERT ? 2 : cross));
        check(ScrollbarWindow::info(pane.window, bar).nPos == 999899, "The scrollbar arrow did not scroll one line");
        SendMessageW(pane.window, WM_CANCELMODE, 0, 0);
        check(GetCapture() != pane.window, "Cancelling a scrollbar press left the pointer captured");
        SendMessageW(pane.window, WM_LBUTTONDOWN, MK_LBUTTON, location);
        check(ScrollbarWindow::info(pane.window, bar).nPos == 999799, "Clicking the scrollbar track did not scroll one page");
        SendMessageW(pane.window, WM_LBUTTONUP, 0, location);
    }
    for (const int bar : {SB_VERT, SB_HORZ}) {
        SCROLLINFO value{sizeof(value), SIF_RANGE | SIF_PAGE, 0, 0, 1, 0, 0};
        SetScrollInfo(pane.window, bar, &value, FALSE);
    }
    const auto empty = Scrollbars::contentRect(pane.window);
    check(empty.right == client.right && empty.bottom == client.bottom, "Hidden bars did not return their space to the content");
}
// Observe every intermediate window change on the UI thread, so a shrink and
// expand within a few milliseconds cannot escape the regression check.
struct QuickOpenWatch {
    HWND window;
    WNDPROC original;
    RECT bounds{};
    int geometryChanges = 0, unexpectedHides = 0, completions = 0;
    bool allowClose = false;
    explicit QuickOpenWatch(HWND value) : window(value) {
        GetWindowRect(window, &bounds);
        MapWindowPoints(nullptr, GetParent(window), reinterpret_cast<POINT*>(&bounds), 2);
        check(SetPropW(window, L"PoppyQuickOpenWatch", this) != FALSE, "Could not observe quick open");
        original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure)));
        check(original != nullptr, "Could not subclass quick open");
    }
    ~QuickOpenWatch() {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        RemovePropW(window, L"PoppyQuickOpenWatch");
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
        auto& self = *static_cast<QuickOpenWatch*>(GetPropW(window, L"PoppyQuickOpenWatch"));
        if (message == WM_WINDOWPOSCHANGING) {
            const auto position = reinterpret_cast<WINDOWPOS*>(b);
            if ((!(position->flags & SWP_NOSIZE) && (position->cx != self.bounds.right - self.bounds.left || position->cy != self.bounds.bottom - self.bounds.top)) ||
                (!(position->flags & SWP_NOMOVE) && (position->x != self.bounds.left || position->y != self.bounds.top))) ++self.geometryChanges;
        }
        if (message == WM_SHOWWINDOW && !a && !self.allowClose) ++self.unexpectedHides;
        const auto result = CallWindowProcW(self.original, window, message, a, b);
        if (message == wmWork) ++self.completions;
        return result;
    }
};
void quickOpenStability() {
    struct Files {
        fs::path root = fs::temp_directory_path() / (L"Poppy popup test " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
        Files() {
            check(fs::create_directory(root), "Could not create popup test files");
            for (const auto name : {L"alpha.txt", L"beta.txt", L"compare.txt", L"gamma.txt"}) {
                std::ofstream file(root / name); file << "test\n";
                check(file.good(), "Could not write a popup test file");
            }
        }
        ~Files() { std::error_code error; fs::remove_all(root, error); }
    } files;
    QuickOpen palette;
    const HWND parent = CreateWindowExW(0, L"STATIC", L"Quick open test", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "Could not create the popup test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    palette.create(parent, GetModuleHandleW(nullptr)); palette.setDpi(96);
    fs::path opened;
    palette.opened = [&](const ResourceId& path) { opened = path.local(); };
    const auto wait = [](const auto& done) {
        const auto start = GetTickCount64();
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (done()) return;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        } while (GetTickCount64() - start < 5000);
        throw std::runtime_error("Quick open did not finish updating");
    };
    const auto accept = [&] { SendMessageW(palette.window(), WM_KEYDOWN, VK_RETURN, 0); return !palette.visible(); };
    palette.setRoot(files.root); palette.show(); wait(accept); // Warm the startup list.
    palette.show();
    QuickOpenWatch observed(palette.window());
    for (const auto ch : std::wstring(L"alpha")) SendMessageW(palette.window(), WM_CHAR, ch, 0);
    observed.allowClose = true; wait(accept); observed.allowClose = false;
    check(opened == files.root / L"alpha.txt", "Quick open opened an outdated match");
    palette.show();
    const auto completed = observed.completions;
    SendMessageW(palette.window(), WM_CHAR, L'z', 0);
    wait([&] { return observed.completions > completed; });
    SendMessageW(palette.window(), WM_KEYDOWN, VK_RETURN, 0);
    check(palette.visible(), "An empty result closed quick open");
    SendMessageW(palette.window(), WM_KEYDOWN, VK_BACK, 0);
    SendMessageW(palette.window(), WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(palette.window(), WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, static_cast<WORD>(-WHEEL_DELTA)), 0);
    SendMessageW(palette.window(), WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    observed.allowClose = true;
    check(accept() && opened == files.root / L"beta.txt", "Quick open lost keyboard selection after clearing the query");
    check(!observed.geometryChanges && !observed.unexpectedHides, "Typing or completing a search resized or hid quick open");

    const std::vector<QuickOpenItem> commands{{"compare", L"Compare Active File With ...", L"alpha.txt"}};
    std::string command;
    fs::path compared;
    palette.commanded = [&](const QuickOpenItem& item) {
        command = item.id;
        palette.chooseFile(L"Compare alpha.txt with ...", files.root / L"alpha.txt",
            [&](const ResourceId& file) { compared = file.local(); });
    };
    const auto type = [&](const std::wstring& query) {
        for (const auto ch : query) SendMessageW(palette.window(), WM_CHAR, ch, 0);
    };
    palette.showCommands(commands); type(L"Compare");
    check(!accept() && command == "compare" && palette.visible(), "A command did not advance to file selection");
    type(L"alpha");
    const auto excluded = observed.completions;
    wait([&] { return observed.completions > excluded; });
    check(!accept() && compared.empty(), "File selection included the active file");
    SendMessageW(palette.window(), WM_KEYDOWN, VK_ESCAPE, 0);
    palette.show(); type(L"beta"); wait(accept);
    check(opened == files.root / L"beta.txt" && compared.empty(), "Cancelling file selection affected the next normal file open");

    command.clear(); palette.showCommands(commands); type(L"Compare Active File With");
    wait([&] { accept(); return command == "compare"; });
    type(L"gamma"); wait(accept);
    check(compared == files.root / L"gamma.txt", "A command's file selection used the normal open callback");

    command.clear(); palette.showCommands(commands); type(L">Compare"); type(L"z");
    check(!accept() && command.empty(), "Enter ran an outdated command after the query changed");
    palette.show(); type(L"Compare"); wait(accept);
    check(opened == files.root / L"compare.txt" && command.empty(), "Switching to file search retained command results");
    palette.close();
}
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--terminal-ime-fixture") return terminalImeFixture(argv[2]);
    const HRESULT com = OleInitialize(nullptr);
    int result = 0;
    try {
        initializeRendering();
        if (argc == 2 && std::string_view(argv[1]) == "--html-preview-runtime") {
            htmlPreviewRuntime();
            std::cout << "WebView2 runtime reuse tests passed\n";
        } else {
            editorBodyFileDrops(); continuousDraggedTab(); terminalDropPreviewClipping();
            shutdownProgress();
            persistentErrorPopup();
            editorBottomPadding(); editorTailFollowing(); largeFileTailPreview();
            csvTables(); htmlPreviewSwitching();
            terminalAsyncStartup(); terminalProcessShutdown(); terminalImeOutput();
            statusBarLimits(); treeMultipleSelection(); commitMessagePopup(); inlineTreeInput(); inlineTreeCreation(); gitCommitInput(); cellPositions(); colorsAndClipping(); imagePreviews(); fractionalMarkers(); smoothIcons(); windowTargets(); editableDiff(); editorCaret(); delayedLoadingMessages(); imeCellRendering(); imeTerminalInsertion(); imeUiRendering(); imePositions(); editorWordSelection(); editorColumnSelection(); editorWordMovement(); editorWordDeletion(); editorLineShortcuts(); editorWrapping(); diffWrapping(); documentFind(); horizontalWheelScrolling(); continuousViewerScrolling(); selectionAutoScrolling(); terminalGitBashSocketEnvironment(); terminalSelectionAutoScrolling(); themedScrollbars(); quickOpenStability();
            std::cout << "Direct2D rendering tests passed\n";
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (SUCCEEDED(com)) OleUninitialize();
    return result;
}
