#include "wsl_dialog.h"

namespace poppy {
namespace {
struct Dialog {
    HWND window = nullptr, distro = nullptr, path = nullptr, user = nullptr;
    std::optional<ResourceId> result;
    bool done = false;
    static std::wstring text(HWND control) { const auto n = GetWindowTextLengthW(control); std::wstring value(static_cast<size_t>(n) + 1, 0); GetWindowTextW(control, value.data(), n + 1); value.resize(n); return value; }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
        auto self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (!self) return DefWindowProcW(window, message, a, b);
        if (message == DM_GETDEFID) return MAKELONG(IDOK, DC_HASDEFID);
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(a) == IDCANCEL)) { self->done = true; return 0; }
        if (message == WM_COMMAND && LOWORD(a) == IDOK) {
            try { self->result = ResourceId::wsl(utf8(text(self->distro)), utf8(text(self->path)), utf8(text(self->user))); self->done = true; }
            catch (const std::exception& error) { MessageBoxW(window, wide(error.what()).c_str(), L"Open WSL folder", MB_OK | MB_ICONWARNING); }
            return 0;
        }
        return DefWindowProcW(window, message, a, b);
    }
};
}
std::optional<ResourceId> chooseWslFolder(HWND owner, const std::vector<std::string>& distributions) {
    Dialog dialog; const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE)); const auto dpi = GetDpiForWindow(owner);
    auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = Dialog::procedure; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); cls.lpszClassName = L"PoppyWslFolder"; RegisterClassW(&cls);
    RECT parent{}; GetWindowRect(owner, &parent);
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP; RECT rect{0,0,px(510),px(265)}; AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    dialog.window = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, L"Open WSL folder", style, parent.left + (parent.right-parent.left-(rect.right-rect.left))/2, parent.top + (parent.bottom-parent.top-(rect.bottom-rect.top))/2, rect.right-rect.left, rect.bottom-rect.top, owner, nullptr, instance, &dialog);
    if (!dialog.window) return {};
    auto control = [&](const wchar_t* type, const wchar_t* label, DWORD flags, int id, int x, int y, int w, int h) { const auto window = CreateWindowExW(type == std::wstring_view(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, label, WS_CHILD | WS_VISIBLE | flags, px(x),px(y),px(w),px(h),dialog.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr); return window; };
    control(L"STATIC", L"Distribution",0,0,18,18,130,22); dialog.distro = control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,10,155,16,335,180);
    for (const auto& distro : distributions) SendMessageW(dialog.distro, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(distro).c_str()));
    SendMessageW(dialog.distro, CB_SETCURSEL, 0, 0);
    control(L"STATIC", L"Linux folder",0,0,18,62,130,22); dialog.path = control(L"EDIT", L"~", WS_TABSTOP|ES_AUTOHSCROLL,11,155,58,335,26); SendMessageW(dialog.path, EM_SETLIMITTEXT, 4096, 0);
    control(L"STATIC", L"User (optional)",0,0,18,106,130,22); dialog.user = control(L"EDIT", L"", WS_TABSTOP|ES_AUTOHSCROLL,12,155,102,335,26); SendMessageW(dialog.user, EM_SETLIMITTEXT, 256, 0);
    control(L"STATIC", L"Use an absolute Linux path, such as /home/user/project.\n~ opens the selected user's home folder.",0,0,18,146,472,42);
    control(L"BUTTON",L"Open",WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,286,215,96,30); control(L"BUTTON",L"Cancel",WS_TABSTOP,IDCANCEL,394,215,96,30);
    EnableWindow(owner,FALSE); ShowWindow(dialog.window,SW_SHOW); SetFocus(dialog.path); SendMessageW(dialog.path,EM_SETSEL,0,-1);
    MSG message{};
    while (!dialog.done) { const auto result = GetMessageW(&message,nullptr,0,0); if (result <= 0) { if (!result) PostQuitMessage(static_cast<int>(message.wParam)); break; } if (!IsDialogMessageW(dialog.window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
    EnableWindow(owner,TRUE); DestroyWindow(dialog.window); SetActiveWindow(owner); return dialog.result;
}
}
