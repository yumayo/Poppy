#include "tree_input.h"
#include "tree_messages.h"
#include <algorithm>
#include <commctrl.h>
#include <imm.h>

namespace poppy {
namespace {
constexpr COLORREF rowBackground = RGB(50, 70, 94), selectionBackground = RGB(75, 99, 129);
constexpr UINT_PTR caretTimer = 1;
bool separator(wchar_t ch) { return ch <= L' ' || ch == L'.' || ch == L'/' || ch == L'\\' || ch == L'-' || ch == L'_'; }
}
HWND TreeInput::create(HWND parent, HINSTANCE instance, const std::wstring& text, COLORREF color) {
    text_ = text; color_ = color; cue_.clear(); undo_.clear(); redo_.clear();
    caret_ = anchor_ = first_ = 0; limit_ = 255; highSurrogate_ = 0; composing_ = false;
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance;
    cls.lpszClassName = treeInputClassName; cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM); cls.style = CS_DBLCLKS;
    RegisterClassW(&cls);
    return CreateWindowExW(0, treeInputClassName, text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(1002), instance, this);
}
void TreeInput::setDpi(UINT dpi) { dpi_ = dpi; renderer_.setDpi(dpi); caretChanged(); }
size_t TreeInput::previous(size_t position) const {
    if (!position) return 0;
    --position;
    if (position && text_[position] >= 0xdc00 && text_[position] <= 0xdfff && text_[position - 1] >= 0xd800 && text_[position - 1] <= 0xdbff) --position;
    return position;
}
size_t TreeInput::next(size_t position) const {
    if (position >= text_.size()) return text_.size();
    const auto ch = text_[position++];
    if (ch >= 0xd800 && ch <= 0xdbff && position < text_.size() && text_[position] >= 0xdc00 && text_[position] <= 0xdfff) ++position;
    return position;
}
size_t TreeInput::word(size_t position, bool forward) const {
    if (forward) {
        while (position < text_.size() && !separator(text_[position])) position = next(position);
        while (position < text_.size() && separator(text_[position])) position = next(position);
    } else {
        while (position && separator(text_[previous(position)])) position = previous(position);
        while (position && !separator(text_[previous(position)])) position = previous(position);
    }
    return position;
}
void TreeInput::replace(std::wstring text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](wchar_t ch) { return ch < L' ' || ch == 127; }), text.end());
    const auto begin = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
    const auto available = limit_ > text_.size() - (end - begin) ? limit_ - (text_.size() - (end - begin)) : 0;
    if (text.size() > available) {
        text.resize(available);
        if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
    }
    if (begin == end && text.empty()) return;
    if (undo_.size() == 32) undo_.erase(undo_.begin());
    undo_.push_back({text_, caret_, anchor_}); redo_.clear();
    text_.replace(begin, end - begin, text); caret_ = anchor_ = begin + text.size(); first_ = std::min(first_, begin);
    DefWindowProcW(window_, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text_.c_str()));
    caretChanged();
}
void TreeInput::undo(bool redo) {
    auto& source = redo ? redo_ : undo_; auto& destination = redo ? undo_ : redo_;
    if (source.empty()) return;
    destination.push_back({text_, caret_, anchor_});
    auto state = std::move(source.back()); source.pop_back();
    text_ = std::move(state.text); caret_ = state.caret; anchor_ = state.anchor;
    DefWindowProcW(window_, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text_.c_str())); caretChanged();
}
size_t TreeInput::hit(int x) const {
    size_t position = first_;
    while (position < text_.size()) {
        const auto after = next(position);
        const int left = renderer_.textWidth(text_.substr(first_, position - first_));
        const int right = renderer_.textWidth(text_.substr(first_, after - first_));
        if (x < (left + right) / 2) break;
        position = after;
    }
    return position;
}
void TreeInput::caretChanged() {
    if (!window_) return;
    RECT rect{}; GetClientRect(window_, &rect);
    first_ = std::min(first_, caret_);
    const int available = std::max(1L, rect.right - MulDiv(2, dpi_, 96));
    if (renderer_.textWidth(text_) <= available) first_ = 0;
    while (first_ < caret_ && renderer_.textWidth(text_.substr(first_, caret_ - first_)) > available) first_ = next(first_);
    const POINT point{renderer_.textWidth(text_.substr(first_, caret_ - first_)), std::max(0L, (rect.bottom - renderer_.textHeight()) / 2)};
    if (GetFocus() == window_) {
        // The hidden system caret also supplies the position to accessibility/IME.
        DestroyCaret(); CreateCaret(window_, nullptr, std::max(1, MulDiv(1, dpi_, 96)), renderer_.textHeight()); SetCaretPos(point.x, point.y);
        const auto context = ImmGetContext(window_);
        if (context) {
            COMPOSITIONFORM form{}; form.dwStyle = CFS_POINT; form.ptCurrentPos = point; ImmSetCompositionWindow(context, &form);
            CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS; candidate.ptCurrentPos = {point.x, rect.bottom};
            ImmSetCandidateWindow(context, &candidate); ImmReleaseContext(window_, context);
        }
        KillTimer(window_, caretTimer);
        const auto blink = GetCaretBlinkTime();
        if (blink && blink != INFINITE) SetTimer(window_, caretTimer, blink, nullptr);
    }
    caretVisible_ = true; InvalidateRect(window_, nullptr, FALSE);
}
void TreeInput::draw(Renderer& renderer, const RECT& rect) const {
    renderer.fill(rect, rowBackground);
    const auto begin = std::max(first_, std::min(anchor_, caret_)), end = std::max(anchor_, caret_);
    const int top = rect.top + std::max(0L, (rect.bottom - rect.top - renderer.textHeight()) / 2);
    if (end > begin) {
        const int left = rect.left + renderer.textWidth(text_.substr(first_, begin - first_));
        const int right = rect.left + renderer.textWidth(text_.substr(first_, end - first_));
        renderer.fill({left, top, std::min<LONG>(right, rect.right), std::min<LONG>(top + renderer.textHeight(), rect.bottom)}, selectionBackground);
    }
    renderer.label(rect, text_.empty() ? cue_ : text_.substr(first_), text_.empty() ? muted : color_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (GetFocus() == window_ && caretVisible_ && !composing_) {
        const int x = rect.left + renderer.textWidth(text_.substr(first_, caret_ - first_));
        if (x < rect.right) renderer.fill({x, top, std::min<LONG>(x + std::max(1, MulDiv(1, dpi_, 96)), rect.right), std::min<LONG>(top + renderer.textHeight(), rect.bottom)}, color_);
    }
}
LRESULT CALLBACK TreeInput::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<TreeInput*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<TreeInput*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_NCDESTROY) { SetWindowLongPtrW(window, GWLP_USERDATA, 0); if (self) self->window_ = nullptr; return DefWindowProcW(window, message, a, b); }
    return self ? self->handle(message, a, b) : DefWindowProcW(window, message, a, b);
}
LRESULT TreeInput::handle(UINT message, WPARAM a, LPARAM b) {
    switch (message) {
    case WM_PAINT: { Renderer::Paint paint(renderer_, window_); if (paint) { RECT rect{}; GetClientRect(window_, &rect); draw(renderer_, rect); } return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_GETTEXTLENGTH: return static_cast<LRESULT>(text_.size());
    case WM_GETTEXT: {
        if (!a || !b) return 0;
        const auto length = std::min(text_.size(), static_cast<size_t>(a - 1));
        std::copy_n(text_.data(), length, reinterpret_cast<wchar_t*>(b)); reinterpret_cast<wchar_t*>(b)[length] = 0;
        return static_cast<LRESULT>(length);
    }
    case WM_SETTEXT:
        text_ = b ? reinterpret_cast<const wchar_t*>(b) : L""; caret_ = anchor_ = text_.size(); first_ = 0; undo_.clear(); redo_.clear(); caretChanged();
        return DefWindowProcW(window_, message, a, b);
    case EM_SETLIMITTEXT: limit_ = a ? static_cast<size_t>(a) : 255; return 0;
    case EM_SETCUEBANNER: cue_ = b ? reinterpret_cast<const wchar_t*>(b) : L""; InvalidateRect(window_, nullptr, FALSE); return TRUE;
    case EM_SETSEL:
        anchor_ = a == static_cast<WPARAM>(-1) ? caret_ : std::min(static_cast<size_t>(a), text_.size());
        caret_ = a == static_cast<WPARAM>(-1) ? caret_ : std::min(static_cast<size_t>(b), text_.size()); caretChanged(); return 0;
    case EM_GETSEL:
        if (a) *reinterpret_cast<DWORD*>(a) = static_cast<DWORD>(std::min(anchor_, caret_));
        if (b) *reinterpret_cast<DWORD*>(b) = static_cast<DWORD>(std::max(anchor_, caret_));
        return MAKELRESULT(std::min(anchor_, caret_), std::max(anchor_, caret_));
    case WM_SIZE: caretChanged(); return 0;
    case WM_SETFOCUS: caretChanged(); return 0;
    case WM_KILLFOCUS: {
        if (composing_) {
            const auto context = ImmGetContext(window_);
            if (context) { ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0); ImmReleaseContext(window_, context); }
            composing_ = false;
        }
        highSurrogate_ = 0; KillTimer(window_, caretTimer); DestroyCaret(); if (finished) finished(false); return 0;
    }
    case WM_TIMER: if (a == caretTimer) { caretVisible_ = !caretVisible_; InvalidateRect(window_, nullptr, FALSE); } return 0;
    case WM_COPY: case WM_CUT:
        if (anchor_ != caret_ && copyText(window_, text_.substr(std::min(anchor_, caret_), std::max(anchor_, caret_) - std::min(anchor_, caret_))) && message == WM_CUT) replace(L"");
        return 0;
    case WM_PASTE: replace(clipboardText(window_)); return 0;
    case WM_CLEAR: if (anchor_ != caret_) replace(L""); return 0;
    case WM_UNDO: undo(false); return TRUE;
    case WM_KEYDOWN: {
        if (composing_) break;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && a == 'A') { anchor_ = 0; caret_ = text_.size(); caretChanged(); return 0; }
        if (ctrl && (a == 'C' || a == 'X' || a == 'V')) { SendMessageW(window_, a == 'C' ? WM_COPY : a == 'X' ? WM_CUT : WM_PASTE, 0, 0); return 0; }
        if (ctrl && (a == 'Z' || a == 'Y')) { undo(a == 'Y' || shift); return 0; }
        if (a == VK_INSERT && (ctrl || shift)) { SendMessageW(window_, ctrl ? WM_COPY : WM_PASTE, 0, 0); return 0; }
        if (a == VK_DELETE && shift && !ctrl) { SendMessageW(window_, WM_CUT, 0, 0); return 0; }
        switch (a) {
        case VK_ESCAPE: case VK_RETURN: if (finished) finished(a == VK_RETURN); return 0;
        case VK_BACK: if (anchor_ == caret_) anchor_ = ctrl ? word(caret_, false) : previous(caret_); replace(L""); return 0;
        case VK_DELETE: if (anchor_ == caret_) anchor_ = ctrl ? word(caret_, true) : next(caret_); replace(L""); return 0;
        case VK_HOME: caret_ = 0; break;
        case VK_END: caret_ = text_.size(); break;
        case VK_LEFT: caret_ = !shift && anchor_ != caret_ ? std::min(anchor_, caret_) : ctrl ? word(caret_, false) : previous(caret_); break;
        case VK_RIGHT: caret_ = !shift && anchor_ != caret_ ? std::max(anchor_, caret_) : ctrl ? word(caret_, true) : next(caret_); break;
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
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (composing_) return 0;
        SetFocus(window_); caret_ = hit(GET_X_LPARAM(b));
        if (message == WM_LBUTTONDBLCLK) { anchor_ = word(caret_, false); caret_ = word(anchor_, true); }
        else if (!(GetKeyState(VK_SHIFT) & 0x8000)) anchor_ = caret_;
        SetCapture(window_); caretChanged(); return 0;
    case WM_MOUSEMOVE: if (GetCapture() == window_) { caret_ = hit(GET_X_LPARAM(b)); caretChanged(); } return 0;
    case WM_LBUTTONUP: if (GetCapture() == window_) ReleaseCapture(); return 0;
    case WM_CONTEXTMENU: {
        if (composing_) return 0;
        const bool selected = anchor_ != caret_;
        const auto menu = CreatePopupMenu(); if (!menu) return 0;
        AppendMenuW(menu, MF_STRING | (undo_.empty() ? MF_GRAYED : 0), 1, L"Undo");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), 2, L"Cut");
        AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), 3, L"Copy");
        AppendMenuW(menu, MF_STRING | (IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED), 4, L"Paste");
        AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), 5, L"Delete");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 6, L"Select All");
        POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        if (point.x == -1 && point.y == -1) { GetCaretPos(&point); ClientToScreen(window_, &point); }
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu);
        if (command == 1) undo(false);
        else if (command >= 2 && command <= 5) SendMessageW(window_, command == 2 ? WM_CUT : command == 3 ? WM_COPY : command == 4 ? WM_PASTE : WM_CLEAR, 0, 0);
        else if (command == 6) { anchor_ = 0; caret_ = text_.size(); caretChanged(); }
        return 0;
    }
    case WM_IME_STARTCOMPOSITION: composing_ = true; caretChanged(); break;
    case WM_IME_ENDCOMPOSITION: composing_ = false; caretChanged(); break;
    case WM_IME_COMPOSITION:
        if (b & GCS_RESULTSTR) {
            const auto context = ImmGetContext(window_);
            const auto bytes = context ? ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0) : 0;
            if (bytes > 0) {
                std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
                ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes); replace(std::move(text));
            }
            if (context) ImmReleaseContext(window_, context);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (GetCapture() == window_) ReleaseCapture();
        KillTimer(window_, caretTimer); renderer_.discard(); return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, a, b);
}
}
