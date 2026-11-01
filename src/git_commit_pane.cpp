#include "git_commit_pane.h"
#include <commctrl.h>
#include <algorithm>

namespace poppy {
void GitCommitPane::create(HWND parent, HINSTANCE instance) {
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance;
    cls.lpszClassName = L"PoppyGitCommitPane"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Commit", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP,
        0, 0, 0, 0, parent, nullptr, instance, this);
    input_ = message_.create(window_, instance, L"", foreground);
    SendMessageW(input_, EM_SETLIMITTEXT, 4096, 0);
    SendMessageW(input_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Commit message (Enter to commit)"));
    message_.finished = [this](bool accept) { if (accept) submit(false); };
    busy(false); setDpi(GetDpiForWindow(parent));
}
void GitCommitPane::setDpi(UINT dpi) { dpi_ = dpi; renderer_.setDpi(dpi); message_.setDpi(dpi); layout(); }
void GitCommitPane::repositories(std::vector<Repository> repositories) {
    repositories_ = std::move(repositories);
    const auto found = std::find_if(repositories_.begin(), repositories_.end(), [this](const auto& repo) { return repo.root == root_; });
    if (found == repositories_.end()) select(0);
    EnableWindow(input_, enabled()); InvalidateRect(window_, nullptr, FALSE);
}
void GitCommitPane::select(size_t index) {
    if (!root_.empty()) drafts_[root_.key()] = message_.text();
    root_ = index < repositories_.size() ? repositories_[index].root : ResourceId{};
    SetWindowTextW(input_, root_.empty() ? L"" : drafts_[root_.key()].c_str());
    InvalidateRect(window_, nullptr, FALSE);
}
void GitCommitPane::busy(bool value) {
    busy_ = value; EnableWindow(input_, enabled()); InvalidateRect(window_, nullptr, FALSE);
}
void GitCommitPane::committed(const ResourceId& root, const std::wstring& message) {
    if (root_ == root) {
        if (message_.text() == message) SetWindowTextW(input_, L"");
    } else if (drafts_[root.key()] == message) drafts_.erase(root.key());
}
RECT GitCommitPane::button(int part) const {
    RECT rect{}; GetClientRect(window_, &rect);
    if (!part) return {0, 0, rect.right, px(26)};
    return {part == 1 ? 0 : std::max(0L, rect.right - px(28)), px(62),
        part == 1 ? std::max(0L, rect.right - px(28)) : rect.right, px(90)};
}
int GitCommitPane::hit(LPARAM point) const {
    for (int part = 0; part < 3; ++part) { const auto rect = button(part); if (PtInRect(&rect, {GET_X_LPARAM(point), GET_Y_LPARAM(point)})) return part; }
    return -1;
}
void GitCommitPane::layout() {
    if (!input_) return;
    RECT rect{}; GetClientRect(window_, &rect);
    SetWindowPos(input_, nullptr, px(6), px(29), std::max(0L, rect.right - px(12)), px(28), SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}
void GitCommitPane::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    RECT rect{}; GetClientRect(window_, &rect); renderer_.fill(rect, panel);
    const auto found = std::find_if(repositories_.begin(), repositories_.end(), [this](const auto& repo) { return repo.root == root_; });
    for (int part = 0; part < 3; ++part) {
        const auto area = button(part);
        if (enabled() && (hover_ == part || pressed_ == part)) renderer_.fill(area, RGB(50, 55, 63));
        const auto color = enabled() ? foreground : muted;
        if (!part) {
            renderer_.label({px(6), area.top, area.right - px(22), area.bottom}, found == repositories_.end() ? L"No repository" : found->label, color);
        } else if (part == 1) renderer_.label(area, busy_ ? L"Committing…" : L"Commit", color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (part != 1) {
            const float x = static_cast<float>(area.right - px(13)), y = static_cast<float>((area.top + area.bottom) / 2);
            renderer_.line(x - px(4), y - px(2), x, y + px(2), color, static_cast<float>(dpi_) / 96);
            renderer_.line(x, y + px(2), x + px(4), y - px(2), color, static_cast<float>(dpi_) / 96);
        }
        if (GetFocus() == window_ && focus_ == part) { auto outline = area; InflateRect(&outline, -1, -1); renderer_.outline(outline, muted); }
    }
    renderer_.outline({0, px(28), rect.right, px(58)}, border);
    renderer_.outline({0, px(62), rect.right, px(90)}, border);
    renderer_.fill({rect.right - px(28), px(62), rect.right - px(28) + 1, px(90)}, border);
}
void GitCommitPane::submit(bool amend) {
    if (enabled() && requested) requested(root_, message_.text(), amend);
}
void GitCommitPane::activate(int part) {
    if (!enabled() || part < 0) return;
    if (part == 1) { submit(false); return; }
    const auto menu = CreatePopupMenu(); if (!menu) return;
    if (!part) {
        for (size_t i = 0; i < repositories_.size(); ++i) {
            auto label = repositories_[i].label;
            for (size_t at = 0; (at = label.find(L'&', at)) != label.npos; at += 2) label.insert(at, 1, L'&');
            AppendMenuW(menu, MF_STRING | (repositories_[i].root == root_ ? MF_CHECKED : 0), i + 1, label.c_str());
        }
    } else AppendMenuW(menu, MF_STRING, 1, L"Commit (Amend)");
    const auto area = button(part); POINT point{area.left, area.bottom}; ClientToScreen(window_, &point);
    // A popup dispatches worker completions; retain the roots represented by it.
    const auto repositories = repositories_; const auto root = root_;
    const auto command = static_cast<size_t>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr));
    DestroyMenu(menu);
    if (!enabled() || !command) return;
    if (!part && command <= repositories.size()) {
        const auto selected = repositories[command - 1].root;
        const auto found = std::find_if(repositories_.begin(), repositories_.end(), [&](const auto& repo) { return repo.root == selected; });
        if (found != repositories_.end()) select(static_cast<size_t>(found - repositories_.begin()));
    } else if (part == 2 && command == 1 && root == root_) submit(true);
}
bool GitCommitPane::key(const MSG& message) {
    if (!contains(message.hwnd) || message.message != WM_KEYDOWN || message.wParam != VK_TAB) return false;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (message.hwnd == input_) { focus_ = shift ? 0 : 1; SetFocus(window_); }
    else if ((focus_ == 0 && !shift) || (focus_ == 1 && shift)) SetFocus(input_);
    else focus_ = (focus_ + (shift ? 2 : 1)) % 3;
    InvalidateRect(window_, nullptr, FALSE); return true;
}
LRESULT CALLBACK GitCommitPane::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<GitCommitPane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<GitCommitPane*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(message, a, b) : DefWindowProcW(window, message, a, b);
}
LRESULT GitCommitPane::handle(UINT message, WPARAM a, LPARAM b) {
    switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: layout(); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track);
        const auto part = hit(b); if (part != hover_) { hover_ = part; InvalidateRect(window_, nullptr, FALSE); } return 0;
    }
    case WM_MOUSELEAVE: hover_ = -1; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        if (enabled() && hit(b) >= 0) { focus_ = pressed_ = hit(b); SetFocus(window_); SetCapture(window_); InvalidateRect(window_, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: {
        const int part = pressed_; pressed_ = -1; if (GetCapture() == window_) ReleaseCapture();
        InvalidateRect(window_, nullptr, FALSE); if (part >= 0 && part == hit(b)) activate(part); return 0;
    }
    case WM_CANCELMODE: if (GetCapture() == window_) ReleaseCapture(); [[fallthrough]];
    case WM_CAPTURECHANGED: pressed_ = -1; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_KEYDOWN:
        if (a == VK_ESCAPE) { SetFocus(input_); return 0; }
        if ((a == VK_RETURN || a == VK_SPACE || a == VK_DOWN) && !(b & (static_cast<LPARAM>(1) << 30))) activate(a == VK_DOWN && focus_ == 1 ? 2 : focus_);
        return 0;
    case WM_CHAR: return 0;
    case WM_DESTROY: renderer_.discard(); return 0;
    default: return DefWindowProcW(window_, message, a, b);
    }
}
}
