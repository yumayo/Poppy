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
void FindBar::show(const std::optional<std::wstring>& seed) {
    visible_ = true;
    if (seed) { query_.clear(); caret_ = anchor_ = inputFirst_ = 0; replace(*seed); }
    anchor_ = 0; caret_ = query_.size();
    ShowWindow(window_, SW_SHOW); SetFocus(window_); caretChanged();
}
void FindBar::close(bool restoreFocus) {
    if (!visible_) return;
    visible_ = false;
    if (composing_) {
        const auto context = ImmGetContext(window_);
        if (context) { ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0); ImmReleaseContext(window_, context); }
        composing_ = false;
    }
    highSurrogate_ = 0;
    const bool hadFocus = GetFocus() == window_;
    ShowWindow(window_, SW_HIDE);
    if (closed) closed();
    if (restoreFocus && hadFocus) SetFocus(GetParent(window_));
}
void FindBar::status(std::wstring text) {
    if (status_ == text) return;
    status_ = std::move(text);
    SetWindowTextW(window_, (L"Find in file: " + status_).c_str());
    InvalidateRect(window_, nullptr, FALSE);
}
RECT FindBar::button(int index) const {
    RECT rect{}; GetClientRect(window_, &rect);
    const int right = static_cast<int>(rect.right) - px(4 + (3 - index) * 26);
    return {right - px(26), px(3), right, rect.bottom - px(3)};
}
RECT FindBar::input() const {
    RECT rect{}; GetClientRect(window_, &rect);
    return {px(8), px(4), std::max<LONG>(px(8), button(0).left - px(100)), rect.bottom - px(4)};
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
    const auto rect = input();
    const POINT point{caretX_, rect.top + px(2)};
    if (GetFocus() == window_) {
        DestroyCaret(); CreateCaret(window_, nullptr, std::max(1, px(1)), renderer_.textHeight()); SetCaretPos(point.x, point.y);
    }
    const auto context = ImmGetContext(window_); if (!context) return;
    COMPOSITIONFORM form{}; form.dwStyle = CFS_POINT; form.ptCurrentPos = point; ImmSetCompositionWindow(context, &form);
    CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS; candidate.ptCurrentPos = {point.x, rect.bottom};
    ImmSetCandidateWindow(context, &candidate); ImmReleaseContext(window_, context);
}
void FindBar::caretChanged() {
    if (!window_) return;
    const auto rect = input();
    inputFirst_ = std::min(inputFirst_, caret_);
    const int available = std::max(1, static_cast<int>(rect.right - rect.left) - px(12));
    while (inputFirst_ < caret_ && renderer_.textWidth(query_.substr(inputFirst_, caret_ - inputFirst_)) > available) inputFirst_ = next(inputFirst_);
    caretX_ = rect.left + px(6) + renderer_.textWidth(query_.substr(inputFirst_, caret_ - inputFirst_));
    imePosition(); InvalidateRect(window_, nullptr, FALSE);
}
void FindBar::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    RECT rect{}; GetClientRect(window_, &rect);
    renderer_.fill(rect, panel);
    const auto box = input(); renderer_.fill(box, background); renderer_.outline(box, GetFocus() == window_ ? accent : border);
    const int origin = box.left + px(6);
    const auto begin = std::max(inputFirst_, std::min(anchor_, caret_)), end = std::max(anchor_, caret_);
    if (end > begin) {
        const int left = origin + renderer_.textWidth(query_.substr(inputFirst_, begin - inputFirst_));
        const int right = origin + renderer_.textWidth(query_.substr(inputFirst_, end - inputFirst_));
        renderer_.fill({left, box.top + 1, std::min<LONG>(right, box.right - px(4)), box.bottom - 1}, RGB(57, 75, 89));
    }
    renderer_.label({origin, box.top, box.right - px(4), box.bottom}, query_.empty() ? L"Find in file" : query_.substr(inputFirst_),
        query_.empty() ? muted : foreground, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (GetFocus() == window_ && !composing_ && caretX_ < box.right)
        renderer_.fill({caretX_, box.top + px(4), caretX_ + std::max(1, px(1)), box.bottom - px(4)}, foreground);
    renderer_.label({box.right + px(4), 0, button(0).left - px(4), rect.bottom}, status_, muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
    case WM_KILLFOCUS: highSurrogate_ = 0; DestroyCaret(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN: {
        if (composing_) break;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && a == 'A') { anchor_ = 0; caret_ = query_.size(); caretChanged(); return 0; }
        if (ctrl && (a == 'C' || a == 'X')) {
            if (anchor_ != caret_) { copyText(window_, query_.substr(std::min(anchor_, caret_), std::max(anchor_, caret_) - std::min(anchor_, caret_))); if (a == 'X') replace(L""); }
            return 0;
        }
        if ((ctrl && a == 'V') || (shift && a == VK_INSERT)) { replace(clipboardText(window_)); return 0; }
        switch (a) {
        case VK_ESCAPE: close(); return 0;
        case VK_RETURN: case VK_F3: if (navigate) navigate(shift); return 0;
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
        if (!composing_ && a >= L' ' && a != 127 && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            const auto ch = static_cast<wchar_t>(a);
            if (ch >= 0xd800 && ch <= 0xdbff) { highSurrogate_ = ch; return 0; }
            std::wstring text;
            if (highSurrogate_ && ch >= 0xdc00 && ch <= 0xdfff) text += highSurrogate_;
            highSurrogate_ = 0; text += ch; replace(std::move(text));
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (composing_) return 0;
        SetFocus(window_);
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        for (int i = 0; i < 4; ++i) {
            const auto area = button(i);
            if (!PtInRect(&area, point)) continue;
            if (i == 0) { matchCase_ = !matchCase_; if (changed) changed(); InvalidateRect(window_, nullptr, FALSE); }
            else if (i == 3) close();
            else if (navigate) navigate(i == 1);
            return 0;
        }
        const auto box = input();
        if (PtInRect(&box, point)) {
            size_t position = inputFirst_;
            while (position < query_.size()) {
                const auto after = next(position);
                const int left = renderer_.textWidth(query_.substr(inputFirst_, position - inputFirst_));
                const int right = renderer_.textWidth(query_.substr(inputFirst_, after - inputFirst_));
                if (point.x - box.left - px(6) < (left + right) / 2) break;
                position = after;
            }
            caret_ = position; if (!(GetKeyState(VK_SHIFT) & 0x8000)) anchor_ = caret_; caretChanged();
        }
        return 0;
    }
    case WM_IME_STARTCOMPOSITION: composing_ = true; imePosition(); InvalidateRect(window_, nullptr, FALSE); break;
    case WM_IME_ENDCOMPOSITION: composing_ = false; caretChanged(); break;
    case WM_IME_COMPOSITION:
        if (b & GCS_RESULTSTR) {
            const auto context = ImmGetContext(window_);
            const LONG bytes = context ? ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0) : 0;
            if (bytes > 0 && visible_) {
                std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes); replace(std::move(text));
            }
            if (context) ImmReleaseContext(window_, context);
            return 0;
        }
        break;
    case WM_DESTROY: renderer_.discard(); return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, a, b);
}
}
