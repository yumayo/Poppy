#include "search_pane.h"
#include "content_search_protocol.h"
#include <algorithm>
#include <chrono>

namespace poppy {
namespace { constexpr UINT searchReady = WM_APP + 90; constexpr UINT_PTR debounce = 1, overlayTimer = 2; }
struct SearchPane::Job {
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    HWND window = nullptr;
    std::deque<std::vector<ContentMatch>> batches;
    size_t matches = 0, bytes = 0;
    std::atomic<bool> limited{false};
    bool done = false, notified = false;
    ContentSearchSummary summary;
};
void SearchPane::notify(const std::shared_ptr<Job>& job) {
    // Caller holds the job mutex. Never block the shared WSL reader on the UI.
    if (!job->cancelled && !job->notified) { job->notified = true; PostMessageW(job->window, searchReady, 0, 0); }
}
bool SearchPane::deliver(const std::shared_ptr<Job>& job, std::vector<ContentMatch> batch) {
    std::lock_guard<std::mutex> lock(job->mutex);
    if (job->cancelled || job->limited) return false;
    size_t count = 0;
    for (const auto& match : batch) {
        const auto bytes = contentMatchBytes(match);
        if (job->matches >= contentResultLimit || bytes > contentResultBytes - job->bytes) { job->limited = true; break; }
        ++count; ++job->matches; job->bytes += bytes;
    }
    batch.resize(count);
    if (job->matches >= contentResultLimit) job->limited = true;
    if (!batch.empty()) job->batches.push_back(std::move(batch));
    notify(job); return !job->limited;
}
void SearchPane::create(HWND parent, HINSTANCE instance) {
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppySearchPane"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Search project", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL | WS_TABSTOP,
        0, 0, 0, 0, parent, nullptr, instance, this);
    input_.create(window_, instance); input_.projectSearch(); input_.show({}, false);
    input_.changed = [this] { refresh(); };
    input_.navigate = [this](bool previous) { move(previous ? -1 : 1); };
    input_.accepted = [this] { accept(); };
    input_.escaped = [this] { cancel(); if (exit) exit(); };
    setDpi(GetDpiForWindow(parent));
}
void SearchPane::setDpi(UINT dpi) { dpi_ = dpi; renderer_.setDpi(dpi); input_.setDpi(dpi); layout(); }
void SearchPane::setContext(const ResourceId& root, std::shared_ptr<ProjectProvider> provider) {
    cancel(); root_ = root; provider_ = std::move(provider); matches_.clear(); selected_ = 0; top_ = 0;
    if (active_) refresh(); else status(L"Type to search this project.");
}
void SearchPane::active(bool visible) {
    if (active_ == visible) return;
    active_ = visible;
    if (visible) refresh(); else cancel();
}
void SearchPane::status(std::wstring text) { status_ = std::move(text); SetWindowTextW(window_, (L"Search project: " + status_).c_str()); InvalidateRect(window_, nullptr, FALSE); }
void SearchPane::cancel() {
    KillTimer(window_, debounce); KillTimer(window_, overlayTimer);
    if (job_) { std::lock_guard<std::mutex> lock(job_->mutex); job_->cancelled = true; }
    job_.reset(); overlays_.clear(); overlayScanner_.reset(); overlayDone_ = true;
    if (worker_) worker_->cancelPending();
    if (!input_.query().empty()) status(L"Search stopped. Press F5 to refresh.");
}
void SearchPane::stop() { cancel(); if (worker_) worker_->stop(); }
void SearchPane::refresh() {
    cancel(); matches_.clear(); selected_ = 0; top_ = 0; layout();
    if (!active_ || input_.query().empty()) { status(L"Type to search this project."); return; }
    status(L"Searching…"); SetTimer(window_, debounce, 150, nullptr);
}
void SearchPane::start() {
    KillTimer(window_, debounce);
    if (!active_ || !provider_ || root_.empty()) { status(L"Open a project to search."); return; }
    if (provider_->state() != ConnectionState::Ready) { status(L"Project unavailable. Reconnect and press F5."); return; }
    options_ = {}; options_.query = utf8(input_.query()); options_.matchCase = input_.matchCase(); options_.respectIgnore = respectIgnore_;
    if (!validContentQuery(options_.query)) { status(L"Enter a single-line search string."); return; }
    overlays_ = overlays ? overlays() : std::vector<SearchOverlay>{};
    for (const auto& overlay : overlays_) options_.excluded.push_back(overlay.relative);
    overlayIndex_ = overlayOffset_ = 0; overlaySummary_ = {}; overlayDone_ = overlays_.empty();
    job_ = std::make_shared<Job>(); job_->window = window_;
    const auto job = job_; const auto options = options_; const auto root = root_; const auto provider = provider_;
    if (!worker_) worker_ = std::make_unique<Worker>(window_);
    worker_->submit(L"search", [job, options, root, provider] {
        ContentSearchSummary summary;
        try { summary = provider->searchContent(root, options, [job](std::vector<ContentMatch> batch) { return deliver(job, std::move(batch)); }, [job] { return job->cancelled.load() || job->limited.load(); }); }
        catch (const std::exception& error) { if (job->limited) summary.limited = true; else summary.error = error.what(); }
        std::lock_guard<std::mutex> lock(job->mutex); job->summary = std::move(summary); job->done = true; notify(job);
    });
    if (!overlayDone_) SetTimer(window_, overlayTimer, 1, nullptr);
    status(L"Searching…");
}
void SearchPane::scanOverlay() {
    if (!job_ || overlayDone_) { KillTimer(window_, overlayTimer); return; }
    if (job_->limited) { overlayDone_ = true; overlays_.clear(); overlayScanner_.reset(); KillTimer(window_, overlayTimer); drain(); return; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    do {
        if (overlayIndex_ >= overlays_.size()) { overlayDone_ = true; overlays_.clear(); KillTimer(window_, overlayTimer); drain(); return; }
        auto& overlay = overlays_[overlayIndex_]; const auto& buffer = *overlay.buffer;
        if (!overlayScanner_) { overlayRevision_ = buffer.revision; overlayOffset_ = 0; overlayScanner_ = std::make_unique<ContentScanner>(overlay.relative, options_, true); }
        if (buffer.revision != overlayRevision_) { refresh(); return; }
        const auto bytes = std::string_view(buffer.document.text).substr(overlayOffset_, 8192);
        const bool final = bytes.empty(); overlayOffset_ += bytes.size();
        if (!overlayScanner_->feed(bytes, final) || final) {
            auto batch = overlayScanner_->take();
            bool complete = true;
            for (size_t p = 0; p < batch.size(); p += 32) {
                const size_t count = std::min<size_t>(32, batch.size() - p);
                std::vector<ContentMatch> part; part.reserve(count);
                for (size_t i = 0; i < count; ++i) part.push_back(std::move(batch[p + i]));
                if (!deliver(job_, std::move(part))) { complete = false; break; }
            }
            overlaySummary_.limited = overlaySummary_.limited || !complete || overlayScanner_->limited();
            ++overlaySummary_.files; ++overlayIndex_; overlayScanner_.reset();
        }
    } while (std::chrono::steady_clock::now() < deadline);
}
void SearchPane::drain() {
    if (!job_) return;
    bool done, limited; ContentSearchSummary summary;
    {
        std::lock_guard<std::mutex> lock(job_->mutex); job_->notified = false;
        for (auto& batch : job_->batches) for (auto& match : batch) matches_.push_back(std::move(match));
        job_->batches.clear(); done = job_->done; summary = job_->summary; limited = job_->limited || summary.limited || overlaySummary_.limited;
    }
    // Keep the selected match stable while parallel batches arrive.
    layout();
    std::wstring text = std::to_wstring(matches_.size()) + L" matches";
    if (limited) text += L" · Results limited";
    if (summary.errors) text += L" · " + std::to_wstring(summary.errors) + L" errors";
    if (!summary.error.empty()) text += L" · " + wide(summary.error);
    if (!done || !overlayDone_) text += L" · Searching…";
    else text += L" · " + std::to_wstring(summary.files + overlaySummary_.files) + L" files";
    if (summary.binary || summary.large) text += L" · Skipped: " + std::to_wstring(summary.binary) + L" binary, " + std::to_wstring(summary.large) + L" large";
    status(std::move(text));
}
int SearchPane::page() const { const auto r = Scrollbars::contentRect(window_); return std::max(1, (static_cast<int>(r.bottom) - body()) / px(48)); }
void SearchPane::layout() {
    if (!window_) return;
    const int count = static_cast<int>(matches_.size()); top_ = std::clamp(top_, 0, std::max(0, count - page()));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, count - 1), static_cast<UINT>(page()), top_, 0}; SetScrollInfo(window_, SB_VERT, &info, FALSE);
    if (input_.window()) input_.layout(0, Scrollbars::contentRect(window_).right, px(64));
    InvalidateRect(window_, nullptr, FALSE);
}
void SearchPane::move(int delta) {
    if (matches_.empty()) return;
    selected_ = static_cast<size_t>(std::clamp<int>(static_cast<int>(selected_) + delta, 0, static_cast<int>(matches_.size()) - 1));
    if (static_cast<int>(selected_) < top_) top_ = static_cast<int>(selected_);
    if (static_cast<int>(selected_) >= top_ + page()) top_ = static_cast<int>(selected_) - page() + 1;
    layout();
}
void SearchPane::accept() { if (selected_ < matches_.size() && opened) opened(matches_[selected_], options_.query, options_.matchCase); }
void SearchPane::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    const auto r = Scrollbars::contentRect(window_); renderer_.fill(r, panel);
    renderer_.label({px(8), px(64), std::min<LONG>(r.right, px(130)), px(88)}, respectIgnore_ ? L"☑ Ignore rules" : L"☐ Ignore rules", muted);
    renderer_.label({px(136), px(64), r.right - px(8), px(88)}, L"Refresh (F5)", muted);
    renderer_.label({px(8), px(88), r.right - px(8), body()}, status_, muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    for (int row = 0; row <= page(); ++row) {
        const size_t index = static_cast<size_t>(top_ + row); if (index >= matches_.size()) break;
        const int y = body() + row * px(48); const auto& match = matches_[index];
        if (index == selected_) renderer_.fill({0, y, r.right, y + px(48)}, rgb(43, 55, 66));
        renderer_.label({px(10), y, r.right - px(8), y + px(23)}, wide(match.relative) + L":" + std::to_wstring(match.line) + L":" + std::to_wstring(match.column), foreground);
        renderer_.label({px(18), y + px(23), r.right - px(8), y + px(46)}, wide(match.snippet), muted);
    }
    scrollbars_.paint(window_, renderer_, panel);
}
LRESULT CALLBACK SearchPane::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<SearchPane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<SearchPane*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(message, a, b) : DefWindowProcW(window, message, a, b);
}
LRESULT SearchPane::handle(UINT message, WPARAM a, LPARAM b) {
    LRESULT result = 0; if (scrollbars_.handle(window_, message, a, b, result)) return result;
    switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: layout(); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case searchReady: drain(); return 0;
    case WM_TIMER: if (a == debounce) start(); else if (a == overlayTimer) scanOverlay(); return 0;
    case WM_KEYDOWN:
        if (a == VK_ESCAPE) { cancel(); if (exit) exit(); }
        else if (a == VK_UP) move(-1); else if (a == VK_DOWN) move(1);
        else if (a == VK_PRIOR) move(-page()); else if (a == VK_NEXT) move(page());
        else if (a == VK_HOME) move(-static_cast<int>(matches_.size())); else if (a == VK_END) move(static_cast<int>(matches_.size()));
        else if (a == VK_RETURN) accept(); else if (a == VK_F5) refresh(); else if (a == VK_TAB) focus();
        return 0;
    case WM_LBUTTONDOWN: {
        const int y = GET_Y_LPARAM(b);
        if (y >= px(64) && y < px(88)) { if (GET_X_LPARAM(b) < px(130)) respectIgnore_ = !respectIgnore_; refresh(); return 0; }
        if (y >= body()) { SetFocus(window_); const size_t index = static_cast<size_t>(top_ + (y - body()) / px(48)); if (index < matches_.size()) { selected_ = index; accept(); } }
        return 0;
    }
    case WM_MOUSEWHEEL: top_ -= GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * 3; layout(); return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window_, SB_VERT, &info);
        switch (LOWORD(a)) { case SB_LINEUP: --top_; break; case SB_LINEDOWN: ++top_; break; case SB_PAGEUP: top_ -= page(); break; case SB_PAGEDOWN: top_ += page(); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: top_ = scrollbars_.trackPosition(SB_VERT, info.nTrackPos); break; case SB_TOP: top_ = 0; break; case SB_BOTTOM: top_ = static_cast<int>(matches_.size()); break; default: return 0; }
        layout(); return 0;
    }
    case WM_DESTROY: stop(); return 0;
    default: break;
    }
    return DefWindowProcW(window_, message, a, b);
}
}
