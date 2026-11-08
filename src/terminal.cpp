#include "terminal.h"
#include "shutdown_dialog.h"
#include "terminal_profile.h"
#include "selection_scroll.h"
#include <imm.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <map>
#include <stdexcept>

namespace poppy {
namespace {
std::vector<Terminal*> liveTerminals;
size_t nextTerminal = 0;
HWND pumpTimerRoot = nullptr;
}
Terminal::~Terminal() {
    stop();
    liveTerminals.erase(std::remove(liveTerminals.begin(), liveTerminals.end(), this), liveTerminals.end());
    if (liveTerminals.empty() && pumpTimerRoot) { KillTimer(pumpTimerRoot, 77); pumpTimerRoot = nullptr; }
}
void Terminal::create(HWND parent, HINSTANCE instance) {
    liveTerminals.push_back(this);
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyTerminal"; cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM); RegisterClassW(&cls);
    // Dock drop previews are siblings; terminal output must not paint over them.
    window_ = CreateWindowExW(0, cls.lpszClassName, L"AI terminal", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 0, parent, nullptr, instance, this);
    model_.notificationError = [this](std::string code) { if (notificationError) notificationError(code); };
    setDpi(GetDpiForWindow(parent));
}
void Terminal::setDpi(UINT dpi) {
    renderer_.setDpi(dpi);
    cellWidth_ = renderer_.cellWidth(); cellHeight_ = renderer_.textHeight() + MulDiv(2, dpi, 96);
    scrollbars(); resize(); imePosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Terminal::setDirectory(const fs::path& directory) {
    if (!started_ && !launching_) { directory_ = directory; error_.clear(); InvalidateRect(window_, nullptr, FALSE); }
}
bool Terminal::start(bool reconnect) {
    if (launching_ || running()) return true;
    stop();
    StartupTask prepare;
    try { if (!reconnect && starting) prepare = starting(); }
    catch (const std::exception& failure) { error_ = wide(failure.what()); stop(); if (launchFailed) launchFailed(error_); return false; }
    error_.clear(); model_ = TerminalModel(model_.columns(), model_.rows());
    // The host answers terminal queries even while no UI is connected.
    model_.notificationError = [this](std::string code) { if (notificationError) notificationError(code); };
    scroll_ = 0; restored_ = false; display_.clearSelection();
    endedNotified_ = false;
    display_ = TerminalDisplay{}; display_.update(model_, scroll_, GetTickCount64(), true);
    scrollbars(); imePosition();
    if (titleChanged) titleChanged();
    if (sessionId_.empty()) {
        GUID guid{}; if (FAILED(CoCreateGuid(&guid))) { error_ = L"Cannot allocate terminal identity"; return false; }
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(guid); ++i) { const auto byte = reinterpret_cast<const unsigned char*>(&guid)[i]; sessionId_ += hex[byte >> 4]; sessionId_ += hex[byte & 15]; }
    }
    stopping_ = false; preserve_ = false; hostFound_ = false; launchFinished_ = false; launching_ = true; launchError_.clear();
    try {
        launcher_.start([this, reconnect, prepare = std::move(prepare), directory = directory_, command = command_, environment = environment_,
                         columns = model_.columns(), rows = model_.rows(), target = window_]() mutable {
            LaunchOptions options;
            try {
                TerminalHostUpdate initial;
                if (reconnect) hostFound_ = host_.attach(sessionId_, initial);
                else {
                    std::error_code directoryError;
                    if (directory.empty() || !fs::is_directory(directory, directoryError)) throw std::runtime_error("Project folder is missing. Restore the folder before starting a shell.");
                    if (prepare) options = prepare();
                    else { options.command = std::move(command); options.environment = std::move(environment); }
                    options.directory = directory; options.columns = columns; options.rows = rows;
                    if (!stopping_) { initial = host_.start(sessionId_, options); hostFound_ = true; }
                }
                if (!stopping_ && hostFound_) {
                    running_ = initial.running; relayRunning_ = initial.relay;
                    { std::lock_guard<std::mutex> lock(mutex_); receivedBytes_ += initial.data.size(); received_.push_back(std::move(initial)); }
                    launchReady_ = std::move(options.ready);
                    reader_.start([this, target] { readHost(target); });
                } else {
                    if (options.failed) options.failed();
                    // A launch already accepted by the host can be retained for Restart.
                    if (!preserve_) host_.close();
                }
            } catch (const std::exception& failure) {
                launchError_ = wide(failure.what());
                stopping_ = true; condition_.notify_all();
                if (options.failed) options.failed();
                try { host_.close(); } catch (...) { }
            }
            launchFinished_ = true;
            PostMessageW(target, wmTerminal, 0, 0);
        });
    } catch (const std::exception& failure) {
        launching_ = false; error_ = wide(failure.what()); stop(); if (launchFailed) launchFailed(error_); return false;
    }
    InvalidateRect(window_, nullptr, FALSE); return true;
}
void Terminal::readHost(HWND target) {
    try {
        while (!stopping_) {
            std::deque<std::string> writes; bool changeSize = false; int columns = 0, rows = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait_for(lock, std::chrono::milliseconds(10), [this] { return stopping_ || (receivedBytes_ <= 512 * 1024 && (resizePending_ || !writes_.empty())); });
                if (stopping_) return;
                if (receivedBytes_ > 512 * 1024) continue;
                writes.swap(writes_); writeBytes_ = 0;
                changeSize = std::exchange(resizePending_, false); columns = requestedColumns_; rows = requestedRows_;
            }
            if (changeSize) host_.resize(columns, rows);
            for (const auto& text : writes) host_.input(text);
            auto update = host_.poll();
            const bool stateChanged = running_.exchange(update.running) != update.running;
            const bool relayChanged = relayRunning_.exchange(update.relay) != update.relay;
            const bool changed = !update.data.empty() || !update.status.empty() || !update.diagnostic.empty() || !update.relayError.empty();
            if (changed) {
                std::lock_guard<std::mutex> lock(mutex_); receivedBytes_ += update.data.size(); received_.push_back(std::move(update));
                if (!notified_) { notified_ = true; PostMessageW(target, wmTerminal, 0, 0); }
            } else if (stateChanged || relayChanged) PostMessageW(target, wmTerminal, 0, 0);
        }
    } catch (const std::exception& failure) {
        running_ = false; relayRunning_ = false;
        TerminalHostUpdate update; update.relayError = failure.what();
        { std::lock_guard<std::mutex> lock(mutex_); received_.push_back(std::move(update)); }
        PostMessageW(target, wmTerminal, 0, 0);
    }
}
void Terminal::completeStart() {
    if (!launching_ || !launchFinished_) return;
    launcher_.join(); launching_ = false;
    error_ = std::move(launchError_);
    if (error_.empty()) {
        started_ = hostFound_;
        if (started_ && connected) connected();
        if (launchReady_) { auto ready = std::move(launchReady_); ready(); }
        // Layout and DPI may have changed while ConPTY was being created.
        { std::lock_guard<std::mutex> lock(mutex_); resizePending_ = started_ && sizeKnown_; }
        condition_.notify_all();
    } else {
        // Remove notification registrations and remote tokens after a failed launch.
        stop();
        if (launchFailed) launchFailed(error_);
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void Terminal::stop(ShutdownDialog* progress) {
    stopSelection();
    ime_.cancel(window_);
    display_.endPreedit();
    stopping_ = true; condition_.notify_all();
    // Preparation owns the relay and process handles until its thread exits.
    const auto joinLaunch = [this] { launcher_.join(); launchReady_ = {}; };
    if (launcher_.joinable()) { if (progress) progress->wait(joinLaunch); else joinLaunch(); }
    launching_ = false;
    if (stopping) stopping();
    if (ended) ended();
    if (window_) KillTimer(window_, 2);
    if (progress) progress->wait([this] { stopProcesses(); }); else stopProcesses();
    if (!shutdownError_.empty() && shutdownFailed) shutdownFailed(std::exchange(shutdownError_, {}));
    started_ = false;
    { std::lock_guard<std::mutex> lock(mutex_); received_.clear(); receivedBytes_ = 0; writes_.clear(); writeBytes_ = 0; notified_ = resizePending_ = false; }
    continuationPosted_ = false;
    display_.update(model_, scroll_, GetTickCount64(), true);
    scrollbars();
}
void Terminal::stopProcesses() {
    stopping_ = true; condition_.notify_all();
    reader_.join();
    shutdownError_.clear();
    try {
        if (preserve_ && host_.connected()) {
            // Retain keystrokes accepted immediately before Restart as well.
            std::deque<std::string> writes;
            { std::lock_guard<std::mutex> lock(mutex_); writes.swap(writes_); writeBytes_ = 0; }
            for (const auto& text : writes) host_.input(text);
        }
    }
    catch (const std::exception& failure) { shutdownError_ = wide(failure.what()); }
    try { host_.close(preserve_, scroll_); }
    catch (const std::exception& failure) { if (shutdownError_.empty()) shutdownError_ = wide(failure.what()); }
    running_ = false; relayRunning_ = false;
}
void Terminal::detach(ShutdownDialog* progress) {
    preserve_ = true;
    stop(progress);
}
void Terminal::send(std::string text, bool userInput) {
    if ((!launching_ && !running()) || text.empty()) return;
    { std::lock_guard<std::mutex> lock(mutex_); if (writeBytes_ + text.size() > 1024 * 1024) { MessageBeep(MB_ICONWARNING); return; } writeBytes_ += text.size(); writes_.push_back(std::move(text)); }
    condition_.notify_all();
    if (userInput) {
        stopSelection();
        const bool selected = hasSelection();
        scroll_ = 0; display_.clearSelection();
        // Echo will invalidate the new frame. Avoid painting the old screen on
        // every key before ConPTY has even processed it.
        if (selected) InvalidateRect(window_, nullptr, FALSE);
        pump();
    }
}
bool Terminal::receive() {
    // Register notification routing and apply readiness on the UI first.
    if (launching_) return false;
    TerminalHostUpdate update;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (received_.empty()) return false;
        auto& first = received_.front();
        if (!first.snapshot && first.data.size() > 8192) {
            update.data = first.data.substr(0, 8192); first.data.erase(0, 8192); receivedBytes_ -= 8192;
        } else { receivedBytes_ -= first.data.size(); update = std::move(first); received_.pop_front(); }
    }
    condition_.notify_all();
    const auto previousTitle = model_.title();
    if (update.snapshot) {
        try { model_ = TerminalModel::fromSnapshot(update.data); }
        catch (const std::exception& failure) { error_ = wide(failure.what()); stop(); if (launchFailed) launchFailed(error_); return false; }
        model_.notificationError = [this](std::string code) { if (notificationError) notificationError(code); };
        if (!restored_) scroll_ = update.scroll;
        restored_ = true; display_.clearSelection();
    } else model_.feed(update.data);
    scroll_ = std::min(scroll_, model_.historySize());
    if (hostStatus) hostStatus(update);
    if (!update.diagnostic.empty() && notificationError) notificationError(update.diagnostic);
    if (model_.title() != previousTitle && titleChanged) titleChanged();
    display_.changed();
    return true;
}
void Terminal::pump() {
    for (const auto terminal : liveTerminals) terminal->completeStart();
    LONGLONG spent = 0;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    HWND root = nullptr;
    // Drain available chunks fairly across sessions, yielding to the message
    // loop after 5 ms of parsing without imposing a delay on the next dispatch.
    size_t idle = 0;
    while (idle < liveTerminals.size() && spent < frequency.QuadPart / 200) {
        if (nextTerminal >= liveTerminals.size()) nextTerminal = 0;
        auto terminal = liveTerminals[nextTerminal++];
        if (!terminal->window_) { ++idle; continue; }
        root = GetAncestor(terminal->window_, GA_ROOT);
        LARGE_INTEGER begin{}, end{}; QueryPerformanceCounter(&begin);
        const bool received = terminal->receive();
        QueryPerformanceCounter(&end); spent += end.QuadPart - begin.QuadPart;
        if (received) idle = 0; else ++idle;
    }
    uint64_t syncDeadline = 0;
    for (const auto terminal : liveTerminals) {
        if (!terminal->window_) continue;
        root = GetAncestor(terminal->window_, GA_ROOT);
        bool queued = false;
        { std::lock_guard<std::mutex> lock(terminal->mutex_); queued = !terminal->received_.empty(); }
        const bool finished = terminal->started_ && !terminal->running();
        if (finished && !terminal->endedNotified_) {
            terminal->endedNotified_ = true;
            terminal->display_.changed();
            if (terminal->ended) terminal->ended();
        }
        if (terminal->display_.update(terminal->model_, terminal->scroll_, GetTickCount64(),
            finished && !queued && terminal->display_.pending())) {
            terminal->scrollbars();
            terminal->imePosition();
            if (IsWindowVisible(terminal->window_)) {
                InvalidateRect(terminal->window_, nullptr, FALSE);
                // Key repeat can keep WM_PAINT behind input in the queue.
                UpdateWindow(terminal->window_);
            }
        }
        const auto deadline = terminal->display_.synchronizationDeadline();
        if (deadline && (!syncDeadline || deadline < syncDeadline)) syncDeadline = deadline;
        // Continue queued parsing through the message queue without a frame
        // timer. Once drained, let the reader notify new output immediately,
        // including an application's synchronized-output end marker.
        std::lock_guard<std::mutex> lock(terminal->mutex_);
        terminal->notified_ = !terminal->received_.empty();
        if (terminal->notified_ && !terminal->continuationPosted_)
            terminal->continuationPosted_ = PostMessageW(terminal->window_, wmTerminal, 1, 0) != FALSE;
    }
    if (pumpTimerRoot) { KillTimer(pumpTimerRoot, 77); pumpTimerRoot = nullptr; }
    // This timer only releases an explicit synchronized update whose end
    // marker never arrives. Ordinary output never waits for a timer.
    const auto now = GetTickCount64();
    if (root && syncDeadline && SetTimer(root, 77, static_cast<UINT>(syncDeadline > now ? syncDeadline - now : 1), nullptr)) pumpTimerRoot = root;
}
void Terminal::resize() {
    if (!window_) return;
    const auto rect = Scrollbars::contentRect(window_);
    if (rect.right < cellWidth_ * 2 || rect.bottom <= 0) return;
    const int columns = std::clamp((static_cast<int>(rect.right) - 12) / cellWidth_, 2, 500);
    const int rows = std::clamp((static_cast<int>(rect.bottom) - 8) / cellHeight_, 1, 200);
    { std::lock_guard<std::mutex> lock(mutex_);
      sizeKnown_ = true;
      if (columns == requestedColumns_ && rows == requestedRows_ && (launching_ || !started_ || (columns == model_.columns() && rows == model_.rows()))) return;
      requestedColumns_ = columns; requestedRows_ = rows; }
    stopSelection();
    if (!launching_ && !started_) model_.resize(columns, rows);
    if (!started_) scroll_ = 0;
    display_.clearSelection();
    scrollbars();
    display_.changed();
    if (launching_ || started_) {
        { std::lock_guard<std::mutex> lock(mutex_); requestedColumns_ = columns; requestedRows_ = rows; resizePending_ = true; }
        condition_.notify_all();
    }
    else display_.update(model_, scroll_, GetTickCount64(), true);
    imePosition();
    InvalidateRect(window_, nullptr, FALSE);
    if (!launching_ && started_) pump();
}
void Terminal::scrollbars() {
    if (!window_) return;
    const int history = static_cast<int>(model_.historySize());
    scroll_ = std::min(scroll_, model_.historySize());
    // Reserve a stable width even without history so new output does not
    // resize ConPTY and cause the running application to redraw its screen.
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL,
        0, history + model_.rows() - 1, static_cast<UINT>(model_.rows()), history - static_cast<int>(scroll_), 0};
    SetScrollInfo(window_, SB_VERT, &info, FALSE);
}
void Terminal::scroll(int offset) {
    pressedLink_.clear();
    scroll_ = static_cast<size_t>(std::clamp(offset, 0, static_cast<int>(model_.historySize())));
    display_.update(model_, scroll_, GetTickCount64());
    scrollbars(); imePosition(); InvalidateRect(window_, nullptr, FALSE);
    // Present each wheel or thumb position while input continues to arrive.
    UpdateWindow(window_);
}
void Terminal::extendSelection(int x, int y) {
    selectionPoint_ = {x, y};
    display_.extendSelection(model_, point(x, y));
    if (hasSelection() || std::abs(x - linkPoint_.x) >= GetSystemMetrics(SM_CXDRAG) ||
        std::abs(y - linkPoint_.y) >= GetSystemMetrics(SM_CYDRAG)) pressedLink_.clear();
    InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_);
}
void Terminal::scrollSelection() {
    if (!selecting_ || GetCapture() != window_) { stopSelection(); return; }
    const auto rect = Scrollbars::contentRect(window_);
    const int top = 4, bottom = std::min(static_cast<int>(rect.bottom), top + display_.rows() * cellHeight_);
    const int rows = selectionScrollRows(selectionPoint_.y, top, bottom, cellHeight_);
    const int offset = std::clamp(static_cast<int>(scroll_) - rows, 0, static_cast<int>(model_.historySize()));
    if (!rows || offset == static_cast<int>(scroll_)) return;
    scroll_ = static_cast<size_t>(offset);
    display_.update(model_, scroll_, GetTickCount64());
    scrollbars(); imePosition();
    extendSelection(selectionPoint_.x, selectionPoint_.y);
}
void Terminal::stopSelection() {
    pressedLink_.clear();
    if (window_) KillTimer(window_, selectionTimer);
    if (!selecting_) return;
    selecting_ = false;
    if (GetCapture() == window_) ReleaseCapture();
}
void Terminal::imePosition() {
    if (ime_.active()) display_.beginPreedit(); else display_.endPreedit();
    const POINT origin{6 + display_.inputCursorX() * cellWidth_, 4 + display_.inputCursorY() * cellHeight_};
    const auto preview = display_.preedit(ime_.text());
    const POINT caret = preview.cells.empty() ? origin : POINT{6 + preview.composition.caretColumn * cellWidth_, 4 + preview.composition.caretRow * cellHeight_};
    positionIme(window_, caret, cellHeight_, renderer_.compositionFont());
}
void Terminal::paste() {
    auto text = utf8(clipboardText(window_)); if (text.empty()) return;
    // Normalize clipboard CRLF to CR for console input.
    std::string input; input.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        input += text[i] == '\n' ? '\r' : text[i];
    }
    if (model_.bracketedPaste()) input = "\x1b[200~" + input + "\x1b[201~";
    send(std::move(input));
}
int Terminal::point(int x, int y) const {
    const int columns = std::max(1, display_.columns()), rows = std::max(1, display_.rows());
    if (y < 4) return 0;
    if (y >= 4 + rows * cellHeight_) return rows * columns;
    return ((y - 4) / cellHeight_) * columns + std::clamp((x - 6 + cellWidth_ / 2) / cellWidth_, 0, columns);
}
std::string Terminal::linkAt(int x, int y) const {
    const auto rect = Scrollbars::contentRect(window_);
    if (!started_ || ime_.active() || x < 6 || y < 4 || x >= rect.right - 6 || y >= rect.bottom) return {};
    // Link hit testing uses the containing cell, not the nearest selection boundary.
    return display_.linkAt((x - 6) / cellWidth_, (y - 4) / cellHeight_);
}
void Terminal::copy() {
    if (!hasSelection()) return;
    if (copyText(window_, wide(display_.selectedText()))) {
        stopSelection();
        display_.clearSelection();
        InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_);
    } else {
        MessageBeep(MB_ICONWARNING);
    }
}
void Terminal::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    auto& dc = renderer_; dc.font(Renderer::Font::Ui);
    const auto rect = Scrollbars::contentRect(window_);
    const auto preview = GetFocus() == window_ ? display_.preedit(ime_.text()) : TerminalPreedit{};
    dc.fill(rect, background);
    if (!started_) {
        RECT text{20, 18, std::max<LONG>(20, rect.right - 20), rect.bottom};
        dc.label(text, launching_ ? L"Starting terminal…" : error_.empty() ? !displayDirectory_.empty() ? L"Click to start the WSL shell.\nThe shell starts only when needed." : L"Click to start PowerShell, then run your AI CLI.\nThe shell starts only when needed." : error_ + L"\nClick to retry.", muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        RECT clip{6, 4, rect.right - 6, rect.bottom};
        const auto [begin, end] = display_.selectionRange();
        std::vector<Cell> visible;
        for (int y = 0; y < display_.rows(); ++y) {
            visible = preview.cells.empty() ? display_.row(y) : preview.cells[y];
            if (begin < end) for (int x = 0; x < display_.columns(); ++x) { const int index = y * display_.columns() + x; if (index >= begin && index < end) visible[x].bg = rgb(50,70,94); }
            const int sy = 4 + y * cellHeight_;
            dc.cells(6, sy, cellWidth_, cellHeight_, visible, display_.columns(), clip);
        }
        if (running() && display_.cursorVisible() && GetFocus() == window_ && ime_.text().text.empty()) {
            const int x = 6 + display_.cursorX() * cellWidth_, y = 4 + display_.cursorY() * cellHeight_;
            if (x + cellWidth_ <= clip.right && y + cellHeight_ <= clip.bottom)
                dc.fill({x, y + cellHeight_ - 2, x + cellWidth_, y + cellHeight_}, accent);
        }
    }
    if (!preview.cells.empty()) {
        const RECT clip{6 + preview.left * cellWidth_, 4 + preview.firstRow * cellHeight_,
            6 + preview.right * cellWidth_, 4 + (preview.lastRow + 1) * cellHeight_};
        dc.composition(preview.composition, {6, 4}, cellWidth_, cellHeight_, clip, preview.compositionBackground);
    }
    scrollbars_.paint(window_, dc, background);
}
LRESULT CALLBACK Terminal::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<Terminal*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<Terminal*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT Terminal::handle(UINT m, WPARAM a, LPARAM b) {
    LRESULT result = 0;
    if (scrollbars_.handle(window_, m, a, b, result)) return result;
    switch (m) {
    case WM_NCDESTROY: { const auto result = DefWindowProcW(window_, m, a, b); window_ = nullptr; return result; }
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: resize(); return 0;
    case wmTerminal: if (a == 1) continuationPosted_ = false; pump(); return 0;
    case WM_TIMER:
        if (a == selectionTimer) scrollSelection();
        else if (a == 2) pump();
        return 0;
    case WM_DESTROY: stopSelection(); ime_.clear(); display_.endPreedit(); if (GetFocus() == window_) DestroyCaret(); return 0;
    case WM_SHOWWINDOW: if (!a) { stopSelection(); ime_.cancel(window_); display_.endPreedit(); } break;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_COPY: copy(); return 0;
    case WM_SETFOCUS: imePosition(); if (focused) focused(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: stopSelection(); ime_.clear(); display_.endPreedit(); highSurrogate_ = 0; DestroyCaret(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(b) == HTCLIENT) {
            POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(window_, &cursor);
            SetCursor(LoadCursorW(nullptr, !selecting_ && !linkAt(cursor.x, cursor.y).empty() ? IDC_HAND : IDC_IBEAM));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(window_);
        ime_.cancel(window_); imePosition();
        if (!started_) { start(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        selectionPoint_ = {GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        display_.beginSelection(point(selectionPoint_.x, selectionPoint_.y));
        selecting_ = true; SetCapture(window_); SetTimer(window_, selectionTimer, 50, nullptr);
        linkPoint_ = selectionPoint_; pressedLink_ = linkAt(linkPoint_.x, linkPoint_.y);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: if (selecting_) extendSelection(GET_X_LPARAM(b), GET_Y_LPARAM(b)); return 0;
    case WM_LBUTTONUP:
        if (selecting_) {
            const int x = GET_X_LPARAM(b), y = GET_Y_LPARAM(b);
            extendSelection(x, y);
            const auto url = pressedLink_;
            const bool open = !url.empty() && GetCapture() == window_ && url == linkAt(x, y);
            stopSelection();
            if (open) {
                const auto target = wide(url);
                SHELLEXECUTEINFOW execute{}; execute.cbSize = sizeof(execute);
                execute.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
                execute.hwnd = window_; execute.lpVerb = L"open"; execute.lpFile = target.c_str(); execute.nShow = SW_SHOWNORMAL;
                // Pass the validated URL directly, without a shell or command arguments.
                if (!ShellExecuteExW(&execute)) {
                    const auto error = winError();
                    MessageBoxW(window_, (target + L"\n\n" + error).c_str(), L"Poppy — Open URL", MB_OK | MB_ICONWARNING);
                }
            }
        }
        return 0;
    case WM_CANCELMODE: case WM_CAPTURECHANGED: stopSelection(); return 0;
    case WM_RBUTTONUP: if (hasSelection()) copy(); else { if (!started_) start(); paste(); } return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * 3;
        scroll(static_cast<int>(scroll_) + delta);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window_, SB_VERT, &info);
        int position = info.nPos;
        switch (LOWORD(a)) {
        case SB_LINEUP: --position; break;
        case SB_LINEDOWN: ++position; break;
        case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = static_cast<int>(model_.historySize()); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: position = scrollbars_.trackPosition(SB_VERT, info.nTrackPos); break;
        default: return 0;
        }
        scroll(static_cast<int>(model_.historySize()) - position);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        if (ime_.active()) return DefWindowProcW(window_, m, a, b);
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (started_ && !running() && a == VK_RETURN && !ctrl && !shift && !alt) { start(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        if (ctrl && shift && !alt && a == 'C') { copy(); return 0; }
        if ((ctrl && shift && a == 'V') || (shift && a == VK_INSERT)) { if (!started_) start(); paste(); return 0; }
        if (!started_ && a != VK_CONTROL && a != VK_SHIFT && a != VK_MENU) { start(); }
        const int modifier = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
        auto cursor = [&](char c) { send(modifier > 1 ? "\x1b[1;" + std::to_string(modifier) + c : std::string(model_.applicationCursor() ? "\x1bO" : "\x1b[") + c); };
        switch (a) {
        case VK_UP: cursor('A'); return 0; case VK_DOWN: cursor('B'); return 0; case VK_RIGHT: cursor('C'); return 0; case VK_LEFT: cursor('D'); return 0;
        case VK_HOME: cursor('H'); return 0; case VK_END: cursor('F'); return 0;
        case VK_DELETE: send("\x1b[3~"); return 0; case VK_INSERT: send("\x1b[2~"); return 0;
        case VK_PRIOR: send("\x1b[5~"); return 0; case VK_NEXT: send("\x1b[6~"); return 0;
        case VK_TAB: if (shift) send("\x1b[Z"); return 0;
        case VK_ESCAPE:
            if (selecting_ || hasSelection()) { stopSelection(); display_.clearSelection(); InvalidateRect(window_, nullptr, FALSE); }
            else send("\x1b");
            return 0;
        default: break;
        }
        if (a >= VK_F1 && a <= VK_F4) { send(std::string("\x1bO") + static_cast<char>('P' + a - VK_F1)); return 0; }
        if (a >= VK_F5 && a <= VK_F12) { const int codes[] = {15,17,18,19,20,21,23,24}; send("\x1b[" + std::to_string(codes[a - VK_F5]) + "~"); return 0; }
        if (ctrl && a == VK_SPACE) { send(std::string(1, '\0')); return 0; }
        return 0;
    }
    case WM_CHAR: case WM_SYSCHAR: {
        if (ime_.active()) return 0;
        if (a == VK_ESCAPE || (a == '\t' && (GetKeyState(VK_SHIFT) & 0x8000))) return 0;
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && (a == 3 || a == 22)) return 0;
        // Handle Ctrl+C's character here so copying cannot also send ETX after
        // clearing the selection. A plain click is not a selection.
        if (a == 3 && !(GetKeyState(VK_MENU) & 0x8000) && hasSelection()) { copy(); return 0; }
        // Windows emits BS for Backspace and DEL for Ctrl+Backspace;
        // Send DEL for one character and Ctrl+W for a word, which is understood
        // by both PSReadLine and Readline (Git Bash / WSL).
        wchar_t c = static_cast<wchar_t>(a);
        if (c == L'\b') c = L'\x7f';
        else if (c == L'\x7f') c = L'\x17';
        if (c >= 0xd800 && c <= 0xdbff) { highSurrogate_ = c; return 0; }
        std::wstring text; if (highSurrogate_) { text += highSurrogate_; highSurrogate_ = 0; } text += c;
        send((m == WM_SYSCHAR ? "\x1b" : "") + utf8(text)); return 0;
    }
    case WM_IME_SETCONTEXT: case WM_INPUTLANGCHANGE: case WM_IME_NOTIFY:
    case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_IME_COMPOSITION:
        if (m == WM_IME_STARTCOMPOSITION) { stopSelection(); display_.endPreedit(); display_.clearSelection(); if (scroll_) scroll(0); }
        return ime_.handle(window_, m, a, b, [this] { imePosition(); },
            [this](std::wstring text) { display_.endPreedit(); send(utf8(text)); });
    default: break;
    }
    return DefWindowProcW(window_, m, a, b);
}
}
