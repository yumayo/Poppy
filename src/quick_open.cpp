#include "quick_open.h"
#include <imm.h>
#include <algorithm>

namespace poppy {
namespace {
std::wstring projectDescription(const ResourceId& path) {
    return path.wsl() ? L"WSL · " + wide(path.location().distro) +
        (path.location().user.empty() ? L"" : L" · " + wide(path.location().user)) + L" · " + path.native() : path.native();
}
}
void QuickOpen::create(HWND parent, HINSTANCE instance, bool projects) {
    projectMode_ = projects;
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance;
    cls.lpszClassName = L"PoppyQuickOpen"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, projects ? L"Recent projects" : L"Go to file", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 0, 0, parent, nullptr, instance, this);
    if (!projects) {
        worker_ = std::make_unique<Worker>(window_);
        indexWorker_ = std::make_unique<Worker>(window_);
    }
}
void QuickOpen::setDpi(UINT dpi) { dpi_ = dpi; renderer_.setDpi(dpi); layout(); }
int QuickOpen::rows() const {
    RECT rect{}; GetClientRect(GetParent(window_), &rect);
    return std::clamp((static_cast<int>(rect.bottom) - px(132)) / px(44), 1, 8);
}
void QuickOpen::layout() {
    if (!visible_) return;
    RECT parent{}; GetClientRect(GetParent(window_), &parent);
    const int width = std::max(0, std::min(px(640), static_cast<int>(parent.right) - px(32)));
    // Keep the popup's surface stable while typing, including zero matches.
    // Only a parent resize or DPI change should resize its Direct2D target.
    const int count = rows();
    if (selected_ < first_) first_ = selected_;
    if (selected_ >= first_ + count) first_ = selected_ - count + 1;
    const int x = (parent.right - width) / 2, y = px(8), height = px(116 + count * 44);
    RECT current{}; GetWindowRect(window_, &current);
    MapWindowPoints(nullptr, GetParent(window_), reinterpret_cast<POINT*>(&current), 2);
    const RECT desired{x, y, x + width, y + height};
    if (!EqualRect(&current, &desired))
        SetWindowPos(window_, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_NOCOPYBITS);
    InvalidateRect(window_, nullptr, FALSE);
}
void QuickOpen::setRoot(const ResourceId& root) {
    close(); ++revision_; ++indexRevision_;
    worker_->cancelPending(); indexWorker_->cancelPending();
    root_ = root; index_.reset(); indexing_ = indexAgain_ = false;
    refresh();
}
void QuickOpen::setProjects(std::vector<ResourceId> projects) {
    projects_ = std::move(projects);
    request();
}
void QuickOpen::show() {
    if (visible_) { SetFocus(window_); anchor_ = 0; caret_ = query_.size(); caretChanged(); return; }
    previousFocus_ = GetFocus(); query_.clear(); caret_ = anchor_ = inputFirst_ = 0;
    selected_ = first_ = 0; wheel_ = 0; highSurrogate_ = 0; result_ = {}; visible_ = true;
    request();
    SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(window_, SW_SHOW); SetFocus(window_);
}
void QuickOpen::close(bool restoreFocus) {
    if (!visible_) return;
    visible_ = false; ++revision_;
    if (worker_) worker_->cancelPending();
    result_ = {}; loading_ = false;
    if (composing_) {
        const auto context = ImmGetContext(window_);
        if (context) { ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0); ImmReleaseContext(window_, context); }
        composing_ = false;
    }
    ShowWindow(window_, SW_HIDE);
    if (restoreFocus && IsWindow(previousFocus_) && IsWindowVisible(previousFocus_)) SetFocus(previousFocus_);
}
void QuickOpen::stop() {
    ++revision_; ++indexRevision_;
    if (indexWorker_) indexWorker_->stop();
    if (worker_) worker_->stop();
}
void QuickOpen::refresh() {
    if (projectMode_ || root_.empty()) return;
    // Publish the current scan even if more changes arrive, then scan again.
    // Typing and closing the palette never cancel an initial file-list load.
    if (indexing_) { indexAgain_ = true; return; }
    const auto revision = ++indexRevision_; const auto root = root_;
    indexing_ = true; layout();
    indexWorker_->submit(L"index", [this, revision, root] {
        auto index = std::make_shared<FileIndex>();
        try { *index = provider_ ? provider_->indexFiles(root, [this, revision] { return indexRevision_.load() != revision; }) : poppy::indexFiles(root.local(), [this, revision] { return indexRevision_.load() != revision; }); }
        catch (const std::exception& error) { index->error = error.what(); }
        if (indexRevision_.load() != revision) return;
        indexWorker_->complete([this, revision, index] {
            if (indexRevision_.load() != revision) return;
            index_ = index; indexing_ = false;
            request();
            if (std::exchange(indexAgain_, false)) refresh();
        });
    });
}
void QuickOpen::changed() {
    request(); caretChanged();
}
void QuickOpen::request() {
    if (!visible_) return;
    if (projectMode_) {
        result_ = {};
        const auto query = utf8(query_);
        for (const auto& path : projects_) {
            const int score = fileSearchScore(utf8(projectDescription(path)), query);
            if (score >= 0) result_.matches.push_back({{}, score, path.serialized()});
        }
        std::stable_sort(result_.matches.begin(), result_.matches.end(),
            [](const FileMatch& a, const FileMatch& b) { return a.score < b.score; });
        selected_ = first_ = 0; layout(); return;
    }
    const auto revision = ++revision_; worker_->cancelPending();
    loading_ = !root_.empty();
    const auto index = index_; const auto query = utf8(query_);
    if (!index || query.empty()) {
        result_ = index ? searchFiles(*index, query) : FileSearchResult{};
        if (index) loading_ = false;
        selected_ = first_ = 0;
        layout(); return;
    }
    // Keep the last completed list until its replacement is ready. accept()
    // blocks activation while loading, so an old match cannot be opened.
    InvalidateRect(window_, nullptr, FALSE);
    worker_->submit(L"files", [this, revision, index, query] {
        auto result = std::make_shared<FileSearchResult>();
        try { *result = searchFiles(*index, query, [this, revision] { return revision_.load() != revision; }); }
        catch (const std::exception& error) { result->error = error.what(); }
        if (revision_.load() != revision) return;
        worker_->complete([this, revision, result] {
            if (!visible_ || revision_.load() != revision) return;
            result_ = std::move(*result); loading_ = false; selected_ = first_ = 0; layout();
        });
    });
}
size_t QuickOpen::previous(size_t position) const {
    if (!position) return 0;
    --position;
    if (position && query_[position] >= 0xdc00 && query_[position] <= 0xdfff && query_[position - 1] >= 0xd800 && query_[position - 1] <= 0xdbff) --position;
    return position;
}
size_t QuickOpen::next(size_t position) const {
    if (position >= query_.size()) return query_.size();
    const auto ch = query_[position++];
    if (ch >= 0xd800 && ch <= 0xdbff && position < query_.size() && query_[position] >= 0xdc00 && query_[position] <= 0xdfff) ++position;
    return position;
}
void QuickOpen::replace(std::wstring text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](wchar_t ch) { return ch < L' ' || ch == 127; }), text.end());
    const auto begin = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
    const size_t available = 512 - (query_.size() - (end - begin));
    if (text.size() > available) {
        text.resize(available);
        if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
    }
    query_.replace(begin, end - begin, text); caret_ = anchor_ = begin + text.size(); changed();
}
void QuickOpen::accept() {
    if (loading_ || selected_ >= result_.matches.size()) return;
    const auto& saved = result_.matches[selected_].exactRelative;
    const auto path = projectMode_ ? ResourceId::parse(saved) : root_ / std::string_view(saved);
    close(false); if (opened) opened(path);
}
void QuickOpen::move(int delta) {
    if (loading_ || result_.matches.empty()) return;
    selected_ = static_cast<size_t>(std::clamp(static_cast<int>(selected_) + delta, 0, static_cast<int>(result_.matches.size()) - 1));
    layout();
}
void QuickOpen::caretChanged() {
    RECT rect{}; GetClientRect(window_, &rect);
    inputFirst_ = std::min(inputFirst_, caret_);
    const int available = std::max(1, static_cast<int>(rect.right) - px(48));
    while (inputFirst_ < caret_ && renderer_.textWidth(query_.substr(inputFirst_, caret_ - inputFirst_)) > available) inputFirst_ = next(inputFirst_);
    caretX_ = px(24) + renderer_.textWidth(query_.substr(inputFirst_, caret_ - inputFirst_));
    imePosition(); InvalidateRect(window_, nullptr, FALSE);
}
void QuickOpen::imePosition() {
    const auto context = ImmGetContext(window_); if (!context) return;
    COMPOSITIONFORM form{}; form.dwStyle = CFS_POINT; form.ptCurrentPos = {caretX_, px(44)};
    ImmSetCompositionWindow(context, &form);
    CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS; candidate.ptCurrentPos = {caretX_, px(76)};
    ImmSetCandidateWindow(context, &candidate); ImmReleaseContext(window_, context);
}
void QuickOpen::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    RECT rect{}; GetClientRect(window_, &rect);
    renderer_.fill(rect, panel); renderer_.outline({0, 0, rect.right - 1, rect.bottom - 1}, border);
    renderer_.fill({1, 1, rect.right - 1, px(2)}, accent);
    renderer_.label({px(16), px(6), rect.right - px(16), px(34)}, projectMode_ ? L"Recent projects" : L"Go to file", foreground);
    const RECT input{px(16), px(38), rect.right - px(16), px(76)};
    renderer_.fill(input, background); renderer_.outline(input, accent);
    const auto begin = std::max(inputFirst_, std::min(anchor_, caret_)), end = std::max(anchor_, caret_);
    if (end > begin) {
        const int left = px(24) + renderer_.textWidth(query_.substr(inputFirst_, begin - inputFirst_));
        const int right = px(24) + renderer_.textWidth(query_.substr(inputFirst_, end - inputFirst_));
        renderer_.fill({left, px(43), std::min<LONG>(right, input.right - px(8)), px(71)}, RGB(57, 75, 89));
    }
    renderer_.label({px(24), input.top, input.right - px(8), input.bottom},
        query_.empty() ? (projectMode_ ? L"Search projects by name or path" : L"Search files by name or path") : query_.substr(inputFirst_), query_.empty() ? muted : foreground,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (GetFocus() == window_ && !composing_) renderer_.fill({caretX_, px(47), caretX_ + std::max(1, px(1)), px(67)}, foreground);
    const int count = std::min(rows(), static_cast<int>(result_.matches.size() - std::min(first_, result_.matches.size())));
    for (int row = 0; row < count; ++row) {
        const auto index = first_ + row; const auto& match = result_.matches[index];
        const int top = px(86 + row * 44);
        if (index == selected_) renderer_.fill({px(8), top, rect.right - px(8), top + px(44)}, RGB(43, 55, 66));
        const auto path = projectMode_ ? ResourceId::parse(match.exactRelative) : root_.at(match.exactRelative);
        renderer_.label({px(20), top, rect.right - px(20), top + px(23)}, path.filename().empty() ? path.native() : path.filename().native(), foreground);
        renderer_.label({px(20), top + px(23), rect.right - px(20), top + px(43)}, projectMode_ ? projectDescription(path) : wide(match.exactRelative), muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
    if (result_.matches.empty()) {
        const auto message = projectMode_ ? (projects_.empty() ? L"No recent projects. Open a folder with Ctrl+O." : L"No matching projects.") : root_.empty() ? L"Open a folder with Ctrl+O to search files." : !index_ ? L"Loading file list…" : loading_ ? L"Searching…" : !result_.error.empty() ? L"Cannot search this folder. Press F5 to retry." : L"No matching files.";
        renderer_.label({px(20), px(86), rect.right - px(20), px(130)}, message, muted);
    }
    const auto hint = indexing_ ? L"Updating file list…" : result_.resultsLimited ? L"Top 100 matches · Type to narrow the results" : L"↑↓ Select    Enter Open    Esc Close";
    renderer_.label({px(16), rect.bottom - px(28), rect.right - px(16), rect.bottom}, hint, muted);
}
LRESULT CALLBACK QuickOpen::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<QuickOpen*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<QuickOpen*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle(message, a, b) : DefWindowProcW(window, message, a, b);
}
LRESULT QuickOpen::handle(UINT message, WPARAM a, LPARAM b) {
    switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: case WM_SETFOCUS: caretChanged(); return 0;
    case WM_KILLFOCUS: close(false); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case wmWork: if (indexWorker_) indexWorker_->dispatch(); if (worker_) worker_->dispatch(); return 0;
    case WM_KEYDOWN: {
        if (!visible_ || composing_) break;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && a == 'A') { anchor_ = 0; caret_ = query_.size(); caretChanged(); return 0; }
        if (ctrl && (a == 'C' || a == 'X')) {
            if (anchor_ != caret_) { copyText(window_, query_.substr(std::min(anchor_, caret_), std::max(anchor_, caret_) - std::min(anchor_, caret_))); if (a == 'X') replace(L""); }
            return 0;
        }
        if ((ctrl && a == 'V') || (shift && a == VK_INSERT)) { replace(clipboardText(window_)); return 0; }
        switch (a) {
        case VK_ESCAPE: close(); return 0;
        case VK_RETURN: accept(); return 0;
        case VK_UP: move(-1); return 0;
        case VK_DOWN: move(1); return 0;
        case VK_PRIOR: move(-rows()); return 0;
        case VK_NEXT: move(rows()); return 0;
        case VK_F5: refresh(); return 0;
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
        if (visible_ && !composing_ && a >= L' ' && a != 127 && !(GetKeyState(VK_CONTROL) & 0x8000)) {
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
        const int y = GET_Y_LPARAM(b), x = GET_X_LPARAM(b);
        if (y >= px(38) && y < px(76)) {
            size_t position = inputFirst_;
            while (position < query_.size()) {
                const auto after = next(position);
                const int left = renderer_.textWidth(query_.substr(inputFirst_, position - inputFirst_));
                const int right = renderer_.textWidth(query_.substr(inputFirst_, after - inputFirst_));
                if (x - px(24) < (left + right) / 2) break;
                position = after;
            }
            caret_ = position; if (!(GetKeyState(VK_SHIFT) & 0x8000)) anchor_ = caret_; caretChanged();
        } else if (y >= px(86) && y < px(86 + rows() * 44)) {
            const auto index = first_ + (y - px(86)) / px(44);
            if (index < result_.matches.size()) { selected_ = index; accept(); }
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(a) & MK_SHIFT) return 0;
        wheel_ += GET_WHEEL_DELTA_WPARAM(a);
        while (wheel_ >= WHEEL_DELTA) { move(-3); wheel_ -= WHEEL_DELTA; }
        while (wheel_ <= -WHEEL_DELTA) { move(3); wheel_ += WHEEL_DELTA; }
        return 0;
    case WM_MOUSEHWHEEL: return 0;
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
    case WM_DESTROY: stop(); renderer_.discard(); return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, a, b);
}
}
