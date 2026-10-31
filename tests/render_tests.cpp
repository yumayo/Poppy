#include "scrollbars.h"
#include "quick_open.h"
#include "viewer.h"
#include "terminal.h"
#include "status_bar.h"
#include "tree.h"
#include <commctrl.h>
#include <imm.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
                hasOverflow |= tip.find(L"Files 1,234/200,000") != tip.npos && tip.find(L"WSL file list") != tip.npos;
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
void smoothIcons() {
    Renderer renderer; Bitmap bitmap;
    for (const UINT dpi : {96u, 120u, 144u, 192u}) {
        renderer.setDpi(dpi);
        check(renderer.begin(bitmap.bitmap.get()), "Could not create the icon test target");
        renderer.fill(bitmap.bounds(), RGB(0, 0, 0));
        const int size = MulDiv(24, dpi, 96);
        const std::vector<Renderer::Icon> icons{Renderer::Icon::Explorer, Renderer::Icon::SourceControl, Renderer::Icon::Branch,
            Renderer::Icon::OpenAllChanges, Renderer::Icon::StageChanges, Renderer::Icon::UnstageChanges, Renderer::Icon::DiscardChanges, Renderer::Icon::StashChanges, Renderer::Icon::OpenFile};
        for (size_t i = 0; i < icons.size(); ++i) {
            const int x = 8 + static_cast<int>(i) * 56;
            renderer.icon(icons[i], {x, 8, x + size, 8 + size}, RGB(255, 255, 255));
        }
        // Icon transforms must not leak into subsequent panel/cell fills.
        renderer.fill({440, 80, 460, 100}, accent);
        finish(renderer); bitmap.read();
        for (size_t i = 0; i < icons.size(); ++i) {
            const int first = 8 + static_cast<int>(i) * 56;
            bool ink = false, blendedEdge = false;
            for (int y = 7; y <= 8 + size; ++y) for (int x = first - 1; x <= first + size; ++x) {
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
            const auto diff = workingDiff(*before, buffer->document, "file.cpp");
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
        key('Z', true); select(6, 6); key('X', true);
        check(buffer->document.text == "head\r\ntail" && clipboardText(viewer.window()) == L"x\r\n", "Cut in a diff did not use the working source line");
        key('Z', true); select(6, 6); key(VK_DOWN, false, true, true);
        check(buffer->document.text == "head\r\ntail\r\nx", "Moving a diff line used patch metadata"); key('Z', true);
        key('A', true); SendMessageW(viewer.window(), WM_CHAR, 'z', 0);
        check(buffer->document.text == "z", "Select All in a diff did not replace the entire working file"); key('Z', true);
        const auto diff = workingDiff(*before, buffer->document, "file.cpp");
        for (const auto& row : diff.rows) if (row.changed && row.before.number) {
            viewer.restore({0, 0, diff.offset(row.before), diff.offset(row.before), false}); break;
        }
        const auto text = buffer->document.text;
        SendMessageW(viewer.window(), WM_CHAR, '!', 0); key(VK_DELETE); key('Z', true);
        check(buffer->document.text == text, "The Before side of a working diff accepted an edit");
        select(6, 7);
        auto changedBase = std::make_shared<Document>(*before); changedBase->text = "different\r\nhead\r\nold\r\ntail"; indexDocument(*changedBase);
        viewer.editDiff(buffer, changedBase, L"file.cpp", true); SendMessageW(viewer.window(), WM_CHAR, 'q', 0);
        check(buffer->document.text == "head\r\nq\r\ntail", "Refreshing the index moved a working selection to a different file offset");
        int saves = 0; viewer.save = [&] { ++saves; }; key('S', true); check(saves == 1, "Ctrl+S in a diff did not request a save"); viewer.save = {};
        viewer.showDiff(std::make_shared<DiffDocument>(diff), L"file.cpp"); SendMessageW(viewer.window(), WM_CHAR, '!', 0);
        check(!viewer.buffer() && buffer->document.text == "head\r\nq\r\ntail", "Read-only comparisons retained an editable buffer");
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
void imePositions() {
    Viewer viewer;
    Terminal terminal, backgroundTerminal;
    QuickOpen quickOpen;
    const auto instance = GetModuleHandleW(nullptr);
    const auto parent = CreateWindowExW(0, L"STATIC", L"IME position test", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 900, 700, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "Cannot create the IME test window");
    struct WindowOwner { HWND value; ~WindowOwner() { DestroyWindow(value); } } owner{parent};
    viewer.create(parent, instance); terminal.create(parent, instance); backgroundTerminal.create(parent, instance);
    quickOpen.create(parent, instance, true);
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
    for (const auto window : {editor, input, background, find, quickOpen.window()})
        context.previous.emplace_back(window, ImmAssociateContext(window, context.value));
    const auto caret = [&](HWND window) {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == window,
            "The focused input lost its native IME caret");
        check(!(info.flags & GUI_CARETBLINKING), "The native IME caret draws over Direct2D");
        return info.rcCaret;
    };
    Renderer metrics;
    const auto checkPosition = [&](HWND window, UINT dpi) {
        const auto rect = caret(window);
        COMPOSITIONFORM composition{}; CANDIDATEFORM candidate{};
        check(ImmGetCompositionWindow(context.value, &composition) && ImmGetCandidateWindow(context.value, 0, &candidate),
            "Cannot inspect the IME window positions");
        check(composition.dwStyle == CFS_POINT && composition.ptCurrentPos.x == rect.left && composition.ptCurrentPos.y == rect.top,
            "The IME composition is detached from the insertion point");
        check(candidate.dwStyle == CFS_CANDIDATEPOS && candidate.ptCurrentPos.x == rect.left && candidate.ptCurrentPos.y == rect.bottom,
            "The IME candidate position is detached from the insertion point");
        LOGFONTW font{};
        check(ImmGetCompositionFontW(context.value, &font) != FALSE, "Cannot inspect the IME composition font");
        metrics.setDpi(dpi);
        check(std::wstring(font.lfFaceName) == L"Consolas" && font.lfHeight == -MulDiv(15, dpi, 96) &&
            font.lfWidth == metrics.cellWidth() && font.lfWeight == FW_NORMAL && !font.lfItalic,
            "The IME composition font does not match the code font and cell width at this DPI");
    };
    const auto resetPosition = [&] {
        COMPOSITIONFORM composition{}; composition.dwStyle = CFS_POINT;
        CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS;
        check(ImmSetCompositionWindow(context.value, &composition) && ImmSetCandidateWindow(context.value, &candidate),
            "Cannot simulate an IME position reset");
        LOGFONTW font{}; font.lfHeight = -8; lstrcpyW(font.lfFaceName, L"Arial");
        check(ImmSetCompositionFontW(context.value, &font) != FALSE, "Cannot simulate an IME font reset");
    };
    const auto refresh = [&](HWND window, UINT dpi) {
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
            checkPosition(window, dpi);
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
    SetFocus(parent);
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    check(GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == nullptr,
        "Leaving an input retained its native IME caret");
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
    const int columns = (Scrollbars::contentRect(window).right - gutter - 2) / width;
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
        const int origin = inlineView ? 140 : 70 + (after ? Scrollbars::contentRect(window).right / 2 + 1 : 0);
        const int y = 34 + (inlineView && after ? height : 0) + height / 2;
        doubleClick(MAKELPARAM(origin + 7 * width + width * 3 / 4, y));
        const std::string word = after ? "new_name" : "old_name";
        selection(diff->patch.text.find(word), word);
        check(viewer.state().after == after, "A double-click selected the wrong diff side");
    }
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
    const int columns = (Scrollbars::contentRect(window).right - 70 - 2) / cell;
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
    TextLayout layout; layout.build(buffer->document, (Scrollbars::contentRect(window).right - 70 - 2) / cell);
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
    const int columns = (Scrollbars::contentRect(window).right - 140 - 2) / cell;
    check(viewer.wordWrap() && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL), "Inline diffs did not inherit word wrapping");
    const auto point = [&](int column, int row) { return MAKELPARAM(140 + column * cell, 34 + row * height + height / 2); };
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
    layout.build(*diff, false, ((Scrollbars::contentRect(window).right - 1) / 2 - 70 - 2) / cell);
    const auto expected = layout.textRow(*diff, layout.rowForOffset(*diff, 1, saved.wrapOffset, false), false);
    check(viewer.state().wrapOffset == expected.begin && viewer.state().anchor == saved.anchor && viewer.state().caret == saved.caret,
        "Changing diff layouts lost the top source position or selection");
    // The top source row extends beyond the right side. Clicking that padding
    // maps to the right line's end while the viewport retains the longer side.
    const int right = Scrollbars::contentRect(window).right / 2 + 1;
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(right + 70, 34 + height / 2));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(right + 70, 34 + height / 2));
    check(viewer.state().anchor == saved.anchor && viewer.state().caret == saved.caret && GetCapture() != window,
        "Clicking the sticky filename must not select the hidden source row");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(right + 70, 34 + height + height / 2));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(right + 70, 34 + height + height / 2));
    const auto padded = viewer.state(); const int splitTop = info().nPos;
    check(padded.after && padded.caret == after + 5 && padded.wrapOffset > 0, "Split padding did not map to the shorter side's end");
    viewer.restore({}); viewer.restore(padded);
    check(info().nPos == splitTop && viewer.state().wrapOffset == padded.wrapOffset && viewer.state().after,
        "Restoring split padding used the selected shorter side's row count");
    MoveWindow(window, 0, 0, 420, 360, FALSE);
    layout.build(*diff, false, ((Scrollbars::contentRect(window).right - 1) / 2 - 70 - 2) / cell);
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
            if (withFind) viewer.find();
            const int header = MulDiv(34, dpi, 96), rowHeight = metrics.textHeight() + MulDiv(3, dpi, 96);
            for (const bool inlineView : {false, true}) {
                const int rightEdge = Scrollbars::contentRect(window).right - header / 3;
                const int x = rightEdge - header * (inlineView ? 1 : 3);
                const auto selection = viewer.state();
                const auto point = MAKELPARAM(x, header * (withFind ? 2 : 1) + rowHeight / 2);
                SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(window, WM_LBUTTONUP, 0, point);
                check(viewer.diffInline() == inlineView && GetCapture() != window && viewer.state().anchor == selection.anchor && viewer.state().caret == selection.caret,
                    "Sticky diff layout buttons lost their hit targets or changed selection after scrolling, resizing, DPI or search");
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
        const int width = inlineView ? Scrollbars::contentRect(window).right : (Scrollbars::contentRect(window).right - 1) / 2;
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
        const int y = (mode >= 2 ? MulDiv(34, dpi, 96) : 0) + 3 * height + height / 2;
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
            for (const auto name : {L"alpha.txt", L"beta.txt", L"gamma.txt"}) {
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
}
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    try {
        initializeRendering();
        statusBarLimits(); treeMultipleSelection(); inlineTreeInput(); cellPositions(); colorsAndClipping(); imagePreviews(); smoothIcons(); windowTargets(); editableDiff(); editorCaret(); imePositions(); editorWordSelection(); editorWordMovement(); editorLineShortcuts(); editorWrapping(); diffWrapping(); documentFind(); horizontalWheelScrolling(); continuousViewerScrolling(); selectionAutoScrolling(); terminalSelectionAutoScrolling(); themedScrollbars(); quickOpenStability();
        std::cout << "Direct2D rendering tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
