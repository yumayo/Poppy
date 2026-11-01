#include "shutdown_dialog.h"
#include <algorithm>
#include <cmath>
#include <exception>

namespace poppy {
ShutdownDialog::ShutdownDialog(HWND owner, HINSTANCE instance) : owner_(owner), started_(GetTickCount64()) {
    WNDCLASSW cls{}; cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpfnWndProc = shadeProcedure; cls.lpszClassName = L"PoppyShutdownShade"; RegisterClassW(&cls);
    cls.lpfnWndProc = procedure; cls.lpszClassName = L"PoppyShutdownDialog"; RegisterClassW(&cls);
    shade_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PoppyShutdownShade", L"", WS_POPUP,
        0, 0, 0, 0, owner, nullptr, instance, this);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Closing Poppy", WS_POPUP,
        0, 0, 0, 0, shade_, nullptr, instance, this);
    if (!shade_ || !window_) {
        if (window_) DestroyWindow(window_);
        if (shade_) DestroyWindow(shade_);
        throw std::runtime_error("Cannot create the shutdown dialog");
    }
    SetLayeredWindowAttributes(shade_, 0, 170, LWA_ALPHA);
    dpi_ = GetDpiForWindow(owner_); renderer_.setDpi(dpi_); shadeRenderer_.setDpi(dpi_);
    layout();
    ownerEnabled_ = IsWindowEnabled(owner_) != FALSE;
    EnableWindow(owner_, FALSE);
    ShowWindow(shade_, SW_SHOWNOACTIVATE); UpdateWindow(shade_);
    ShowWindow(window_, SW_SHOW); UpdateWindow(window_);
    SetTimer(window_, 1, 60, nullptr);
}
ShutdownDialog::~ShutdownDialog() {
    // Restore the owner before destroying its active modal window.
    if (ownerEnabled_ && IsWindow(owner_)) EnableWindow(owner_, TRUE);
    if (window_) DestroyWindow(window_);
    if (shade_) DestroyWindow(shade_);
    if (quit_) PostQuitMessage(*quit_);
}
void ShutdownDialog::layout() {
    RECT owner{}; GetWindowRect(owner_, &owner);
    SetWindowPos(shade_, nullptr, owner.left, owner.top, owner.right-owner.left, owner.bottom-owner.top, SWP_NOZORDER | SWP_NOACTIVATE);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = std::min(px(520), static_cast<int>(monitor.rcWork.right-monitor.rcWork.left));
    const int height = std::min(px(388), static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top));
    const int x = std::clamp(static_cast<int>(owner.left+(owner.right-owner.left-width)/2), static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right)-width);
    const int y = std::clamp(static_cast<int>(owner.top+(owner.bottom-owner.top-height)/2), static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom)-height);
    SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}
void ShutdownDialog::stage(Stage stage, std::wstring context) {
    stage_ = stage; context_ = std::move(context);
    InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_);
}
void ShutdownDialog::detail(std::wstring text) {
    detail_ = std::move(text);
    SetWindowTextW(window_, (L"Closing Poppy — " + detail_).c_str());
    InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_);
}
void ShutdownDialog::wait(const std::function<void()>& task) {
    std::exception_ptr failure;
    NativeThread worker([&] { try { task(); } catch (...) { failure = std::current_exception(); } });
    const HANDLE thread = worker.native_handle();
    while (MsgWaitForMultipleObjectsEx(1, &thread, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0+1) {
        MSG message{};
        // Bound each batch so continuous output cannot starve completion.
        for (unsigned count = 0; count < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) { quit_ = static_cast<int>(message.wParam); continue; }
            if (message.hwnd == window_ || message.hwnd == shade_) {
                TranslateMessage(&message); DispatchMessageW(&message);
            } else if (message.message == WM_PAINT) {
                // Retain the background's last frame. Its models may be waiting
                // for I/O, and shutdown must not reenter application callbacks.
                ValidateRect(message.hwnd, nullptr);
            }
        }
    }
    worker.join();
    if (failure) std::rethrow_exception(failure);
}
void ShutdownDialog::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    RECT client{}; GetClientRect(window_, &client);
    renderer_.fill(client, panel); renderer_.outline(client, border, static_cast<float>(px(1)));
    const int left = px(28), right = client.right-px(28);
    renderer_.fillRounded({left, px(26), left+px(24), px(30)}, accent, static_cast<float>(px(2)));
    renderer_.label({left, px(40), right, px(74)}, L"Closing Poppy", foreground);
    renderer_.label({left, px(76), right, px(102)}, L"Please wait while your workspace shuts down.", muted);
    const wchar_t* steps[]{L"Save workspace", L"Stop projects and terminals", L"Release resources"};
    const unsigned frame = static_cast<unsigned>((GetTickCount64()-started_)/60);
    for (int i = 0; i < 3; ++i) {
        const int y = px(122+i*34), cx = left+px(8), cy = y+px(12);
        const bool done = i < static_cast<int>(stage_), active = i == static_cast<int>(stage_);
        if (done) {
            renderer_.line(static_cast<float>(cx-px(5)), static_cast<float>(cy), static_cast<float>(cx-px(1)), static_cast<float>(cy+px(4)), accent, static_cast<float>(px(2)));
            renderer_.line(static_cast<float>(cx-px(1)), static_cast<float>(cy+px(4)), static_cast<float>(cx+px(6)), static_cast<float>(cy-px(5)), accent, static_cast<float>(px(2)));
        } else if (active) {
            for (unsigned dot = 0; dot < 8; ++dot) {
                const double angle = dot*3.141592653589793/4;
                const int x = cx+static_cast<int>(std::cos(angle)*px(6)), dy = cy+static_cast<int>(std::sin(angle)*px(6));
                renderer_.fill({x-px(1), dy-px(1), x+px(1), dy+px(1)}, accent, 0.2f+0.1f*((dot+8-frame%8)%8));
            }
        } else renderer_.ellipse({cx-px(5), cy-px(5), cx+px(5), cy+px(5)}, border, static_cast<float>(px(1)));
        renderer_.label({left+px(28), y, right, y+px(24)}, steps[i], done || active ? foreground : muted);
    }
    renderer_.fillRounded({left, px(238), right, px(318)}, background, static_cast<float>(px(6)));
    renderer_.label({left+px(12), px(245), right-px(12), px(269)}, context_, muted);
    renderer_.label({left+px(12), px(272), right-px(12), px(312)}, detail_, foreground, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    const auto seconds = (GetTickCount64()-started_)/1000;
    renderer_.label({left, px(332), right-px(80), px(356)}, L"The window will close automatically.", muted);
    renderer_.label({right-px(70), px(332), right, px(356)}, std::to_wstring(seconds)+L"s", muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    const int trackWidth = right-left, segment = std::max(1, trackWidth/5);
    renderer_.fillRounded({left, px(366), right, px(369)}, border, static_cast<float>(px(1)));
    const int offset = static_cast<int>(frame%60)*(trackWidth+segment)/60-segment;
    renderer_.fill({left+std::max(0, offset), px(366), left+std::min(trackWidth, offset+segment), px(369)}, accent);
}
LRESULT CALLBACK ShutdownDialog::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto self = reinterpret_cast<ShutdownDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<ShutdownDialog*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (!self) return DefWindowProcW(window, message, a, b);
    switch (message) {
    case WM_PAINT: self->paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_DPICHANGED: self->dpi_ = HIWORD(a); self->renderer_.setDpi(self->dpi_); self->shadeRenderer_.setDpi(self->dpi_); self->layout(); return 0;
    case WM_CLOSE: return 0;
    case WM_SYSCOMMAND: if ((a & 0xfff0) == SC_CLOSE) return 0; break;
    case WM_DESTROY: KillTimer(window, 1); return 0;
    }
    return DefWindowProcW(window, message, a, b);
}
LRESULT CALLBACK ShutdownDialog::shadeProcedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto self = reinterpret_cast<ShutdownDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<ShutdownDialog*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (self) switch (message) {
    case WM_PAINT: { Renderer::Paint paint(self->shadeRenderer_, window); if (paint) { RECT rect{}; GetClientRect(window, &rect); self->shadeRenderer_.fill(rect, RGB(0, 0, 0)); } return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: SetForegroundWindow(self->window_); return 0;
    case WM_CLOSE: return 0;
    }
    return DefWindowProcW(window, message, a, b);
}
}
