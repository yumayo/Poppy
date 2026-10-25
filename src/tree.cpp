#include "tree.h"
#include <algorithm>
#include <commctrl.h>

namespace poppy {
void Tree::create(HWND parent, HINSTANCE instance, const wchar_t* title, int id) {
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = treeClassName;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.style = CS_DBLCLKS;
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, treeClassName, title, WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
    setDpi(GetDpiForWindow(parent));
}
void Tree::setDpi(UINT dpi, int rowHeight) {
    const UINT previous = dpi_;
    dpi_ = dpi; renderer_.setDpi(dpi);
    rowHeight_ = std::max(renderer_.textHeight(), MulDiv(rowHeight, dpi, 96));
    indent_ = MulDiv(18, dpi, 96); left_ = MulDiv(left_, dpi, previous);
    widths_.clear(); changed();
}
TreeItem Tree::insert(TreeItem parent, std::wstring text, bool expandable, uintptr_t data, COLORREF color) {
    const auto id = model_.insert(parent, std::move(text), expandable, data, color);
    if (id) changed();
    return id;
}
void Tree::label(TreeItem id, std::wstring text, COLORREF color) {
    const auto node = item(id); if (!node) return;
    if (node->text == text && node->color == color) return;
    if (node->text != text) widths_.erase(id);
    model_.label(id, std::move(text), color); changed();
}
void Tree::release(const std::vector<std::pair<TreeItem, uintptr_t>>& items) {
    for (const auto& pair : items) { if (pair.first == renaming_) finishRename(false); widths_.erase(pair.first); if (removed) removed(pair.second); }
}
void Tree::erase(TreeItem id) { release(model_.erase(id)); changed(); }
void Tree::clearChildren(TreeItem id) { release(model_.eraseChildren(id)); changed(); }
void Tree::clear() { release(model_.clear()); widths_.clear(); search_.clear(); top_ = left_ = wheel_ = 0; changed(); }
void Tree::sort(TreeItem parent, const std::function<bool(uintptr_t, uintptr_t)>& compare) { model_.sort(parent, compare); changed(); }
void Tree::expand(TreeItem id, bool value) {
    if (!model_.expand(id, value)) return;
    // Change state before notifying: a loading folder can immediately collapse.
    // Callbacks may delete children or synchronously insert a cached result.
    Batch batch(*this);
    if (expanded) expanded(id, value);
    changed();
}
void Tree::select(TreeItem id, bool activate) {
    if (!model_.select(id)) return;
    ensureVisible(); changed();
    if (id && activate && activated) activated(id);
}
int Tree::row(TreeItem id) const {
    const auto& rows = model_.visible();
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const TreeModel::Row& row) { return row.item == id; });
    return found == rows.end() ? -1 : static_cast<int>(found - rows.begin());
}
TreeItem Tree::hit(int y) const {
    const auto client = Scrollbars::contentRect(window_);
    const auto& rows = model_.visible();
    const int index = top_ + y / rowHeight_;
    return y >= 0 && y < client.bottom && index >= 0 && index < static_cast<int>(rows.size()) ? rows[index].item : 0;
}
int Tree::page() const { const auto r = Scrollbars::contentRect(window_); return std::max(1, static_cast<int>(r.bottom) / rowHeight_); }
void Tree::ensureVisible() {
    const int index = row(selection());
    if (index < 0) return;
    if (index < top_) top_ = index;
    else if (index >= top_ + page()) top_ = index - page() + 1;
}
void Tree::changed() { if (!batch_) layout(); }
void Tree::layout() {
    if (!window_ || layingOut_) return;
    layingOut_ = true;
    const auto& rows = model_.visible();
    int extent = 0;
    for (const auto& row : rows) {
        auto found = widths_.find(row.item);
        if (found == widths_.end()) found = widths_.emplace(row.item, renderer_.textWidth(item(row.item)->text)).first;
        extent = std::max(extent, static_cast<int>(row.depth + 1) * indent_ + found->second + MulDiv(12, dpi_, 96));
    }
    // Each bar can change the space available to the other.
    for (int pass = 0; pass < 3; ++pass) {
        const auto r = Scrollbars::contentRect(window_);
        const int count = static_cast<int>(rows.size()), height = page(), width = std::max(1L, r.right);
        top_ = std::clamp(top_, 0, std::max(0, count - height));
        left_ = std::clamp(left_, 0, std::max(0, extent - width));
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, count - 1), static_cast<UINT>(height), top_, 0};
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, extent - 1), static_cast<UINT>(width), left_, 0};
        SetScrollInfo(window_, SB_VERT, &vertical, FALSE); SetScrollInfo(window_, SB_HORZ, &horizontal, FALSE);
    }
    layingOut_ = false; layoutRename(); InvalidateRect(window_, nullptr, FALSE);
}
void Tree::rename(TreeItem id, const std::wstring& name) {
    finishRename(false); if (!item(id)) return;
    select(id, false); renaming_ = id;
    rename_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", name.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(1002), reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE)), nullptr);
    if (!rename_) { renaming_ = 0; return; }
    SetWindowSubclass(rename_, renameProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(rename_, EM_SETLIMITTEXT, 255, 0); layoutRename();
    const auto extension = item(id)->expandable ? std::wstring::npos : name.find_last_of(L'.');
    SendMessageW(rename_, EM_SETSEL, 0, extension == std::wstring::npos || extension == 0 ? -1 : static_cast<LPARAM>(extension)); SetFocus(rename_);
}
void Tree::layoutRename() {
    if (!rename_) return;
    const int index = row(renaming_); if (index < top_ || index >= top_ + page()) { finishRename(false); return; }
    const auto& value = model_.visible()[static_cast<size_t>(index)]; const auto r = Scrollbars::contentRect(window_);
    const int left = std::clamp(static_cast<int>(value.depth + 1) * indent_ - left_, 0, static_cast<int>(std::max(0L, r.right - MulDiv(80, dpi_, 96))));
    SetWindowPos(rename_, HWND_TOP, left, (index - top_) * rowHeight_, std::max(0L, r.right - left), rowHeight_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void Tree::finishRename(bool commit) {
    if (!rename_) return;
    const auto id = renaming_; const auto edit = std::exchange(rename_, nullptr); renaming_ = 0;
    wchar_t name[256]{}; GetWindowTextW(edit, name, 256);
    const bool focus = GetFocus() == edit; DestroyWindow(edit); if (focus) SetFocus(window_);
    if (commit && item(id) && renameCommitted) renameCommitted(id, name);
}
LRESULT CALLBACK Tree::renameProcedure(HWND window, UINT message, WPARAM a, LPARAM b, UINT_PTR, DWORD_PTR data) {
    auto& self = *reinterpret_cast<Tree*>(data);
    if (message == WM_KEYDOWN && (a == VK_RETURN || a == VK_ESCAPE)) { self.finishRename(a == VK_RETURN); return 0; }
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, renameProcedure, 1);
    return DefSubclassProc(window, message, a, b);
}
void Tree::scroll(int vertical, int horizontal) { top_ += vertical; left_ += horizontal; changed(); }
void Tree::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    const auto r = Scrollbars::contentRect(window_);
    renderer_.fill(r, panel);
    const auto& rows = model_.visible();
    const bool focus = GetFocus() == window_;
    const int padding = MulDiv(5, dpi_, 96), arrow = MulDiv(4, dpi_, 96);
    for (int index = top_; index < static_cast<int>(rows.size()) && (index - top_) * rowHeight_ < r.bottom; ++index) {
        const auto& row = rows[index]; const auto node = item(row.item);
        const int y = (index - top_) * rowHeight_, x = static_cast<int>(row.depth) * indent_ - left_;
        const bool selected = selection() == row.item;
        if (selected) {
            renderer_.fill({0, y, r.right, y + rowHeight_}, focus ? RGB(50, 70, 94) : RGB(40, 47, 58));
            if (focus) renderer_.fill({0, y, MulDiv(2, dpi_, 96), y + rowHeight_}, accent);
        }
        if (node->expandable) {
            const float cx = static_cast<float>(x + indent_ / 2), cy = static_cast<float>(y + rowHeight_ / 2);
            const float stroke = static_cast<float>(std::max(1, MulDiv(1, dpi_, 96)));
            if (node->expanded) {
                renderer_.line(cx - arrow, cy - arrow / 2.0f, cx, cy + arrow / 2.0f, muted, stroke);
                renderer_.line(cx, cy + arrow / 2.0f, cx + arrow, cy - arrow / 2.0f, muted, stroke);
            } else {
                renderer_.line(cx - arrow / 2.0f, cy - arrow, cx + arrow / 2.0f, cy, muted, stroke);
                renderer_.line(cx + arrow / 2.0f, cy, cx - arrow / 2.0f, cy + arrow, muted, stroke);
            }
        }
        renderer_.label({x + indent_ + padding, y, std::max<LONG>(r.right, x + indent_ + widths_[row.item] + padding), y + rowHeight_},
            node->text, node->color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    scrollbars_.paint(window_, renderer_, panel);
}
LRESULT CALLBACK Tree::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<Tree*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<Tree*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT Tree::handle(UINT m, WPARAM a, LPARAM b) {
    LRESULT result = 0;
    if (scrollbars_.handle(window_, m, a, b, result)) return result;
    switch (m) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: changed(); return 0;
    case WM_COMMAND: if (LOWORD(a) == 1002 && HIWORD(a) == EN_KILLFOCUS) finishRename(false); return 0;
    case WM_CONTEXTMENU: {
        if (!renameRequested) return 0;
        POINT screen{GET_X_LPARAM(b), GET_Y_LPARAM(b)}; auto id = selection();
        if (screen.x == -1 && screen.y == -1) { screen = {indent_, (row(id) - top_ + 1) * rowHeight_}; ClientToScreen(window_, &screen); }
        else { auto point = screen; ScreenToClient(window_, &point); id = hit(point.y); if (id) select(id, false); }
        if (!id) return 0;
        HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING | (canRename && !canRename(id) ? MF_GRAYED : 0), 1, L"Rename\tF2");
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, window_, nullptr); DestroyMenu(menu);
        if (command == 1 && item(id) && (!canRename || canRename(id))) renameRequested(id);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS: if (!selection() && child()) select(child(), false); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        const auto id = hit(GET_Y_LPARAM(b)); if (!id) return 0;
        const auto& row = model_.visible()[static_cast<size_t>(this->row(id))];
        const int x = GET_X_LPARAM(b) + left_, edge = static_cast<int>(row.depth) * indent_;
        if (item(id)->expandable && x >= edge && x < edge + indent_) expand(id, !item(id)->expanded);
        else select(id);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const auto id = hit(GET_Y_LPARAM(b));
        if (id && item(id)->expandable) {
            const auto& row = model_.visible()[static_cast<size_t>(this->row(id))];
            if (GET_X_LPARAM(b) + left_ >= static_cast<int>(row.depth + 1) * indent_) expand(id, !item(id)->expanded);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        wheel_ += GET_WHEEL_DELTA_WPARAM(a);
        const int steps = wheel_ / WHEEL_DELTA; wheel_ %= WHEEL_DELTA;
        scroll(-steps * (lines == WHEEL_PAGESCROLL ? page() : static_cast<int>(lines))); return 0;
    }
    case WM_VSCROLL: case WM_HSCROLL: {
        const bool vertical = m == WM_VSCROLL;
        SCROLLINFO info{sizeof(info), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window_, vertical ? SB_VERT : SB_HORZ, &info);
        int position = vertical ? top_ : left_;
        const int step = vertical ? 1 : indent_;
        switch (LOWORD(a)) {
        case SB_LINEUP: position -= step; break; case SB_LINEDOWN: position += step; break;
        case SB_PAGEUP: position -= static_cast<int>(info.nPage); break; case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: position = scrollbars_.trackPosition(vertical ? SB_VERT : SB_HORZ, info.nTrackPos); break;
        case SB_TOP: position = 0; break; case SB_BOTTOM: position = info.nMax; break;
        default: return 0;
        }
        if (vertical) top_ = position; else left_ = position;
        changed(); return 0;
    }
    case WM_KEYDOWN: {
        const auto& rows = model_.visible(); if (rows.empty()) return 0;
        const auto id = selection(); const auto node = item(id);
        int index = row(id); if (index < 0) index = 0;
        switch (a) {
        case VK_F2: if (id && renameRequested && (!canRename || canRename(id))) renameRequested(id); return 0;
        case VK_LEFT: if (node && node->expanded) expand(id, false); else if (node && node->parent) select(node->parent); return 0;
        case VK_RIGHT: if (node && node->expandable) { if (!node->expanded) expand(id, true); else if (child(id)) select(child(id)); } return 0;
        case VK_RETURN: case VK_SPACE: if (node && node->expandable) expand(id, !node->expanded); else select(id); return 0;
        case VK_UP: --index; break; case VK_DOWN: if (id) ++index; break;
        case VK_PRIOR: index -= page(); break; case VK_NEXT: index += page(); break;
        case VK_HOME: index = 0; break; case VK_END: index = static_cast<int>(rows.size()) - 1; break;
        default: return 0;
        }
        select(rows[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(rows.size()) - 1))].item); return 0;
    }
    case WM_CHAR: {
        if (a <= 32 || a == 127) return 0;
        const auto now = GetTickCount64();
        if (now - searchTime_ > 1000 || search_.size() >= 256 || (search_.size() == 1 && search_[0] == static_cast<wchar_t>(a))) search_.clear();
        searchTime_ = now;
        search_ += static_cast<wchar_t>(a);
        const auto& rows = model_.visible(); const int current = row(selection());
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto index = (static_cast<size_t>(std::max(0, current)) + (search_.size() == 1 ? 1 : 0) + i) % rows.size();
            const auto& text = item(rows[index].item)->text;
            if (text.size() >= search_.size() && CompareStringOrdinal(text.data(), static_cast<int>(search_.size()), search_.data(), static_cast<int>(search_.size()), TRUE) == CSTR_EQUAL) { select(rows[index].item); break; }
        }
        return 0;
    }
    case treeGetNext: switch (static_cast<TreeRelation>(a)) {
        case TreeRelation::Root: return static_cast<LRESULT>(child());
        case TreeRelation::Child: return static_cast<LRESULT>(child(static_cast<TreeItem>(b)));
        case TreeRelation::Next: return static_cast<LRESULT>(next(static_cast<TreeItem>(b)));
        case TreeRelation::Parent: return static_cast<LRESULT>(model_.parent(static_cast<TreeItem>(b)));
        } return 0;
    case treeGetCount: return static_cast<LRESULT>(model_.size());
    case treeGetExpanded: { const auto node = item(static_cast<TreeItem>(a)); return node && node->expanded; }
    case treeSelect: select(static_cast<TreeItem>(a)); return selection() == static_cast<TreeItem>(a);
    case treeGetSelection: return static_cast<LRESULT>(selection());
    case treeGetRow: return row(static_cast<TreeItem>(a));
    case treeGetRowHeight: return rowHeight_;
    default: return DefWindowProcW(window_, m, a, b);
    }
}
}
