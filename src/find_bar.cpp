#include "find_bar.h"
#include <algorithm>
#include <imm.h>

namespace poppy {
void FindBar::create(HWND parent, HINSTANCE instance) {
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance;
    cls.lpszClassName = L"PoppyFindBar"; cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Find in file", WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP,
        0, 0, 0, 0, parent, nullptr, instance, this);
}
void FindBar::setDpi(UINT dpi) { dpi_ = dpi; renderer_.setDpi(dpi); caretChanged(); }
void FindBar::layout(int top, int width, int height) {
    MoveWindow(window_, 0, top, width, height, TRUE); caretChanged();
}
void FindBar::show(const std::optional<std::wstring>& seed, bool focus) {
    ime_.cancel(window_);
    visible_ = true;
    if (seed) { query_.clear(); caret_ = anchor_ = 0; line_.reset(); replace(*seed); }
    anchor_ = 0; caret_ = query_.size();
    ShowWindow(window_, SW_SHOW); if (focus) SetFocus(window_); caretChanged();
}
void FindBar::restore(std::wstring query, bool matchCase) {
    ime_.cancel(window_); highSurrogate_ = 0;
    query_ = std::move(query); matchCase_ = matchCase;
    anchor_ = 0; caret_ = query_.size(); line_.reset(); caretChanged();
}
void FindBar::close(bool restoreFocus) {
    if (!visible_) return;
    visible_ = false;
    ime_.cancel(window_);
    highSurrogate_ = 0;
    const bool hadFocus = GetFocus() == window_;
    ShowWindow(window_, SW_HIDE);
    if (closed) closed();
    if (restoreFocus && hadFocus) SetFocus(GetParent(window_));
}
void FindBar::status(std::wstring text) {
    if (status_ == text) return;
    status_ = std::move(text);
    SetWindowTextW(window_, ((compact_ ? L"Search project: " : L"Find in file: ") + status_).c_str());
    InvalidateRect(window_, nullptr, FALSE);
}
RECT FindBar::button(int index) const {
    RECT rect{}; GetClientRect(window_, &rect);
    const int right = static_cast<int>(rect.right) - px(4 + (3 - index) * 26);
    return {right - px(26), px(compact_ ? 35 : 3), right, rect.bottom - px(3)};
}
RECT FindBar::input() const {
    RECT rect{}; GetClientRect(window_, &rect);
    return {px(8), px(4), std::max<LONG>(px(8), compact_ ? rect.right - px(8) : button(0).left - px(100)), compact_ ? px(30) : rect.bottom - px(4)};
}
size_t FindBar::previous(size_t position) const {
    if (!position) return 0;
    --position;
    if (position && query_[position] >= 0xdc00 && query_[position] <= 0xdfff && query_[position - 1] >= 0xd800 && query_[position - 1] <= 0xdbff) --position;
    return position;
}
size_t FindBar::next(size_t position) const {
    if (position >= query_.size()) return query_.size();
    const auto ch = query_[position++];
    if (ch >= 0xd800 && ch <= 0xdbff && position < query_.size() && query_[position] >= 0xdc00 && query_[position] <= 0xdfff) ++position;
    return position;
}
void FindBar::replace(std::wstring text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](wchar_t ch) { return ch < L' ' || ch == 127; }), text.end());
    const auto begin = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
    const size_t available = 512 - (query_.size() - (end - begin));
    if (text.size() > available) {
        text.resize(available);
        if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
    }
    query_.replace(begin, end - begin, text); caret_ = anchor_ = begin + text.size();
    caretChanged(); if (changed) changed();
}
void FindBar::imePosition() {
    positionIme(window_, line_.caret(), line_.height(), renderer_.compositionFont(Renderer::Font::Ui));
}
void FindBar::caretChanged() {
    if (!window_) return;
    auto rect = input(); rect.left += px(6); rect.right -= px(4);
    line_.update(renderer_, query_, caret_, anchor_, ime_.text(), rect);
    imePosition(); InvalidateRect(window_, nullptr, FALSE);
}
void FindBar::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    RECT rect{}; GetClientRect(window_, &rect);
    renderer_.fill(rect, panel);
    const auto box = input(); renderer_.fill(box, background); renderer_.outline(box, GetFocus() == window_ ? accent : border);
    if (query_.empty() && ime_.text().text.empty())
        renderer_.label({box.left + px(6), box.top, box.right - px(4), box.bottom}, compact_ ? L"Search project" : L"Find in file", muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    line_.draw(renderer_, foreground, RGB(57, 75, 89), GetFocus() == window_);
    renderer_.label({compact_ ? px(8) : box.right + px(4), compact_ ? px(32) : 0, button(0).left - px(4), rect.bottom}, status_, muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    for (int i = 0; i < 4; ++i) {
        const auto area = button(i);
        if (i == 0 && matchCase_) renderer_.fill(area, RGB(50, 70, 94));
        renderer_.label(area, i == 0 ? L"Aa" : i == 1 ? L"↑" : i == 2 ? L"↓" : L"×", foreground, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    renderer_.fill({0, rect.bottom - 1, rect.right, rect.bottom}, border);
}
LRESULT CALLBACK FindBar::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<FindBar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<FindBar*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(message, a, b) : DefWindowProcW(window, message, a, b);
}
LRESULT FindBar::handle(UINT message, WPARAM a, LPARAM b) {
    switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(b) == HTCLIENT) {
            POINT point{}; GetCursorPos(&point); ScreenToClient(window_, &point);
            const auto box = input();
            SetCursor(LoadCursorW(nullptr, PtInRect(&box, point) ? IDC_IBEAM : IDC_ARROW)); return TRUE;
        }
        break;
    case WM_SIZE: caretChanged(); return 0;
    case WM_SETFOCUS: caretChanged(); if (focused) focused(); return 0;
    case WM_KILLFOCUS: ime_.cancel(window_); highSurrogate_ = 0; DestroyCaret(); caretChanged(); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN: {
        if (ime_.active()) break;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && a == 'A') { anchor_ = 0; caret_ = query_.size(); caretChanged(); return 0; }
        if (ctrl && (a == 'C' || a == 'X')) {
            if (anchor_ != caret_) { copyText(window_, query_.substr(std::min(anchor_, caret_), std::max(anchor_, caret_) - std::min(anchor_, caret_))); if (a == 'X') replace(L""); }
            return 0;
        }
        if ((ctrl && a == 'V') || (shift && a == VK_INSERT)) { replace(clipboardText(window_)); return 0; }
        switch (a) {
        case VK_ESCAPE: if (escaped) escaped(); else close(); return 0;
        case VK_RETURN: if (accepted) accepted(); else if (navigate) navigate(shift); return 0;
        case VK_F3: if (navigate) navigate(shift); return 0;
        case VK_UP: if (navigate) navigate(true); return 0;
        case VK_DOWN: if (navigate) navigate(false); return 0;
        case VK_TAB: SetFocus(GetParent(window_)); return 0;
        case VK_BACK: if (anchor_ == caret_) anchor_ = ctrl ? 0 : previous(caret_); replace(L""); return 0;
        case VK_DELETE: if (anchor_ == caret_) anchor_ = ctrl ? query_.size() : next(caret_); replace(L""); return 0;
        case VK_HOME: caret_ = 0; break;
        case VK_END: caret_ = query_.size(); break;
        case VK_LEFT: caret_ = !shift && anchor_ != caret_ ? std::min(anchor_, caret_) : ctrl ? 0 : previous(caret_); break;
        case VK_RIGHT: caret_ = !shift && anchor_ != caret_ ? std::max(anchor_, caret_) : ctrl ? query_.size() : next(caret_); break;
        default: return 0;
        }
        if (!shift) anchor_ = caret_;
        caretChanged(); return 0;
    }
    case WM_CHAR:
        if (!ime_.active() && a >= L' ' && a != 127 && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            const auto ch = static_cast<wchar_t>(a);
            if (ch >= 0xd800 && ch <= 0xdbff) { highSurrogate_ = ch; return 0; }
            std::wstring text;
            if (highSurrogate_ && ch >= 0xdc00 && ch <= 0xdfff) text += highSurrogate_;
            highSurrogate_ = 0; text += ch; replace(std::move(text));
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (ime_.active()) return 0;
        SetFocus(window_);
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        for (int i = 0; i < 4; ++i) {
            const auto area = button(i);
            if (!PtInRect(&area, point)) continue;
            if (i == 0) { matchCase_ = !matchCase_; if (changed) changed(); InvalidateRect(window_, nullptr, FALSE); }
            else if (i == 3) { if (escaped) escaped(); else close(); }
            else if (navigate) navigate(i == 1);
            return 0;
        }
        const auto box = input();
        if (PtInRect(&box, point)) {
            const auto position = line_.hit(point.x);
            caret_ = position; if (!(GetKeyState(VK_SHIFT) & 0x8000)) anchor_ = caret_; caretChanged();
        }
        return 0;
    }
    case WM_IME_SETCONTEXT: case WM_INPUTLANGCHANGE: case WM_IME_NOTIFY:
    case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_IME_COMPOSITION:
        return ime_.handle(window_, message, a, b, [this] { caretChanged(); },
            [this](std::wstring text) { if (visible_) replace(std::move(text)); });
    case WM_SHOWWINDOW: if (!a) { ime_.cancel(window_); highSurrogate_ = 0; caretChanged(); } break;
    case WM_DESTROY: ime_.clear(); if (GetFocus() == window_) DestroyCaret(); renderer_.discard(); return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, a, b);
}
}
