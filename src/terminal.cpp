#include "terminal.h"
#include "terminal_profile.h"
#include "selection_scroll.h"
#include <imm.h>
#include <algorithm>
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
    window_ = CreateWindowExW(0, cls.lpszClassName, L"AI terminal", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 0, parent, nullptr, instance, this);
    model_.reply = [this](std::string response) { send(std::move(response), false); };
    setDpi(GetDpiForWindow(parent));
}
void Terminal::setDpi(UINT dpi) {
    renderer_.setDpi(dpi);
    cellWidth_ = renderer_.cellWidth(); cellHeight_ = renderer_.textHeight() + MulDiv(2, dpi, 96);
    scrollbars(); resize(); imePosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Terminal::setDirectory(const fs::path& directory) {
    if (!started_) { directory_ = directory; error_.clear(); InvalidateRect(window_, nullptr, FALSE); }
}
bool Terminal::start() {
    if (running()) return true;
    stop();
    std::error_code directoryError;
    if (directory_.empty() || !fs::is_directory(directory_, directoryError)) { error_ = L"Project folder is missing. Restore the folder before starting a shell."; return false; }
    try { if (starting) starting(); }
    catch (const std::exception& failure) { error_ = wide(failure.what()); stop(); return false; }
    error_.clear(); model_ = TerminalModel(model_.columns(), model_.rows()); model_.reply = [this](std::string s) { send(std::move(s), false); };
    scroll_ = 0; display_.clearSelection();
    endedNotified_ = false;
    display_ = TerminalDisplay{}; display_.update(model_, scroll_, GetTickCount64(), true);
    scrollbars(); imePosition();
    if (titleChanged) titleChanged();
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, nullptr, 0)) { error_ = winError(); stop(); return false; }
    Handle childInput(read); input_.reset(write);
    if (!CreatePipe(&read, &write, nullptr, 0)) { error_ = winError(); stop(); return false; }
    output_.reset(read); Handle childOutput(write);
    const HRESULT hr = CreatePseudoConsole({static_cast<SHORT>(model_.columns()), static_cast<SHORT>(model_.rows())}, childInput.value, childOutput.value, 0, &console_);
    if (FAILED(hr)) { error_ = L"ConPTY could not start (HRESULT " + std::to_wstring(static_cast<unsigned long>(hr)) + L")."; stop(); return false; }
    childInput.reset(); childOutput.reset();
    // Read concurrently even during creation/teardown, as ClosePseudoConsole can emit output.
    stopping_ = false;
    reader_.start([this] {
        char buffer[8192]; DWORD count = 0;
        while (ReadFile(output_.value, buffer, sizeof(buffer), &count, nullptr) && count) {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || received_.size() + count <= 256 * 1024; });
            if (!stopping_) {
                received_.append(buffer, count);
                if (!notified_) { notified_ = true; PostMessageW(window_, wmTerminal, 0, 0); }
            }
        }
        PostMessageW(window_, wmTerminal, 0, 0);
    });
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size); auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) { error_ = winError(); stop(); return false; }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console_, sizeof(console_), nullptr, nullptr)) { error_ = winError(); DeleteProcThreadAttributeList(attributes); stop(); return false; }
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); si.lpAttributeList = attributes;
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    const auto args = command_.empty() ? terminalShellCommand(TerminalShell::PowerShell, system) : command_;
    const auto& shell = args.front();
    std::wstring command = processCommandLine(args);
    job_.reset(CreateJobObjectW(nullptr, nullptr)); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { error_ = winError(); DeleteProcThreadAttributeList(attributes); stop(); return false; }
    PROCESS_INFORMATION pi{};
    struct Less { bool operator()(const std::wstring& a, const std::wstring& b) const { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; } };
    std::map<std::wstring, std::wstring, Less> inherited;
    if (auto block = GetEnvironmentStringsW()) {
        for (auto entry = block; *entry; entry += wcslen(entry)+1) {
            const std::wstring value(entry); const auto split = value.find(L'=', value.front() == L'=' ? 1 : 0);
            if (split != std::wstring::npos) inherited[value.substr(0,split)] = value.substr(split+1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& value : environment_) inherited[value.first] = value.second;
    // Replace the socket entry so its Linux path is passed without translation.
    const auto previous = inherited[L"WSLENV"]; std::wstring wslenv;
    for (size_t p = 0; p < previous.size();) {
        const auto end = previous.find(L':', p);
        const auto token = previous.substr(p, end == std::wstring::npos ? end : end-p);
        const auto name = token.substr(0, token.find(L'/'));
        if (name != L"AICONTAINER_EVENT_SOCKET" && !token.empty()) {
            if (!wslenv.empty()) wslenv += L':';
            wslenv += token;
        }
        if (end == std::wstring::npos) break;
        p = end + 1;
    }
    if (!inherited[L"AICONTAINER_EVENT_SOCKET"].empty()) {
        if (!wslenv.empty()) wslenv += L':';
        wslenv += L"AICONTAINER_EVENT_SOCKET/u";
        // Git Bash converts environment paths before launching native children
        // such as wsl.exe, before WSLENV can forward the Linux socket path.
        auto& excluded = inherited[L"MSYS2_ENV_CONV_EXCL"];
        if (excluded != L"*") {
            if (!excluded.empty() && excluded.back() != L';') excluded += L';';
            excluded += L"AICONTAINER_EVENT_SOCKET";
        }
    }
    inherited[L"WSLENV"] = std::move(wslenv);
    std::vector<wchar_t> environment;
    for (const auto& value : inherited) { const auto entry = value.first + L"=" + value.second; environment.insert(environment.end(),entry.begin(),entry.end()); environment.push_back(0); }
    environment.push_back(0);
    const BOOL ok = CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, environment.data(), directory_.empty() ? nullptr : directory_.c_str(), &si.StartupInfo, &pi);
    const DWORD error = ok ? 0 : GetLastError(); DeleteProcThreadAttributeList(attributes);
    if (!ok) { error_ = L"Could not start " + shell + L": " + winError(error); stop(); return false; }
    process_.reset(pi.hProcess); Handle thread(pi.hThread);
    if (!AssignProcessToJobObject(job_.value, process_.value)) { error_ = winError(); TerminateProcess(process_.value, 1); stop(); return false; }
    writer_.start([this] {
        for (;;) {
            std::string text; bool changeSize = false; COORD size{};
            { std::unique_lock<std::mutex> lock(mutex_); condition_.wait(lock, [&] { return stopping_ || resizePending_ || !writes_.empty(); }); if (stopping_) return;
              if (resizePending_) { changeSize = true; size = {static_cast<SHORT>(requestedColumns_), static_cast<SHORT>(requestedRows_)}; resizePending_ = false; }
              if (!writes_.empty()) { text = std::move(writes_.front()); writes_.pop_front(); writeBytes_ -= text.size(); } }
            if (changeSize) ResizePseudoConsole(console_, size);
            size_t offset = 0;
            while (offset < text.size() && !stopping_) {
                DWORD countWritten = 0;
                if (!WriteFile(input_.value, text.data() + offset, static_cast<DWORD>(text.size() - offset), &countWritten, nullptr) || !countWritten) return;
                offset += countWritten;
            }
        }
    });
    ResumeThread(thread.value); started_ = true; InvalidateRect(window_, nullptr, FALSE); return true;
}
void Terminal::stop() {
    stopSelection();
    ime_.cancel(window_);
    if (stopping) stopping();
    if (ended) ended();
    if (window_) KillTimer(window_, 2);
    stopping_ = true; condition_.notify_all();
    if (job_) TerminateJobObject(job_.value, 0);
    if (writer_.joinable()) {
        // Repeat cancellation to cover a stop arriving just before WriteFile begins.
        while (WaitForSingleObject(writer_.native_handle(), 0) == WAIT_TIMEOUT) { CancelSynchronousIo(writer_.native_handle()); WaitForSingleObject(writer_.native_handle(), 10); }
        writer_.join();
    }
    // The reader keeps draining while ConPTY sends its final output.
    if (console_) { ClosePseudoConsole(console_); console_ = nullptr; }
    if (reader_.joinable()) { CancelSynchronousIo(reader_.native_handle()); reader_.join(); }
    input_.reset(); output_.reset(); process_.reset(); job_.reset(); started_ = false;
    { std::lock_guard<std::mutex> lock(mutex_); received_.clear(); writes_.clear(); writeBytes_ = 0; notified_ = resizePending_ = false; }
    continuationPosted_ = false;
    display_.update(model_, scroll_, GetTickCount64(), true);
    scrollbars();
}
void Terminal::send(std::string text, bool userInput) {
    if (!running() || text.empty()) return;
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
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t count = std::min<size_t>(received_.size(), 8192);
        text.assign(received_.data(), count); received_.erase(0, count);
    }
    if (text.empty()) return false;
    condition_.notify_all();
    const auto previousTitle = model_.title();
    model_.feed(text); scroll_ = std::min(scroll_, model_.historySize());
    if (model_.title() != previousTitle && titleChanged) titleChanged();
    display_.changed();
    return true;
}
void Terminal::pump() {
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
    if (columns == model_.columns() && rows == model_.rows()) return;
    stopSelection();
    model_.resize(columns, rows); scroll_ = 0; display_.clearSelection();
    scrollbars();
    display_.changed();
    if (console_) {
        { std::lock_guard<std::mutex> lock(mutex_); requestedColumns_ = columns; requestedRows_ = rows; resizePending_ = true; }
        condition_.notify_all();
    }
    else display_.update(model_, scroll_, GetTickCount64(), true);
    imePosition();
    InvalidateRect(window_, nullptr, FALSE);
    if (console_) pump();
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
    scroll_ = static_cast<size_t>(std::clamp(offset, 0, static_cast<int>(model_.historySize())));
    display_.update(model_, scroll_, GetTickCount64());
    scrollbars(); imePosition(); InvalidateRect(window_, nullptr, FALSE);
    // Present each wheel or thumb position while input continues to arrive.
    UpdateWindow(window_);
}
void Terminal::extendSelection(int x, int y) {
    selectionPoint_ = {x, y};
    display_.extendSelection(model_, point(x, y));
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
    if (window_) KillTimer(window_, selectionTimer);
    if (!selecting_) return;
    selecting_ = false;
    if (GetCapture() == window_) ReleaseCapture();
}
void Terminal::imePosition() {
    const auto rect = Scrollbars::contentRect(window_);
    const RECT clip{6, 4, rect.right - 6, rect.bottom};
    const POINT origin{6 + display_.cursorX() * cellWidth_, 4 + display_.cursorY() * cellHeight_};
    positionIme(window_, renderer_.compositionCaret(ime_.text(), origin, cellWidth_, cellHeight_, clip), cellHeight_, renderer_.compositionFont());
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
    dc.fill(rect, background);
    if (!started_) {
        RECT text{20, 18, std::max<LONG>(20, rect.right - 20), rect.bottom};
        dc.label(text, error_.empty() ? !displayDirectory_.empty() ? L"Click to start the WSL shell.\nThe shell starts only when needed." : L"Click to start PowerShell, then run your AI CLI.\nThe shell starts only when needed." : error_ + L"\nClick to retry.", muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        RECT clip{6, 4, rect.right - 6, rect.bottom};
        const auto [begin, end] = display_.selectionRange();
        std::vector<Cell> visible;
        for (int y = 0; y < display_.rows(); ++y) {
            visible = display_.row(y);
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
    if (GetFocus() == window_) {
        const POINT origin{6 + display_.cursorX() * cellWidth_, 4 + display_.cursorY() * cellHeight_};
        dc.composition(ime_.text(), origin, cellWidth_, cellHeight_, {6, 4, rect.right - 6, rect.bottom});
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
    case WM_DESTROY: stopSelection(); ime_.clear(); if (GetFocus() == window_) DestroyCaret(); return 0;
    case WM_SHOWWINDOW: if (!a) { stopSelection(); ime_.cancel(window_); } break;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_COPY: copy(); return 0;
    case WM_SETFOCUS: imePosition(); if (focused) focused(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: stopSelection(); ime_.clear(); highSurrogate_ = 0; DestroyCaret(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window_);
        if (!started_) { start(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        selectionPoint_ = {GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        display_.beginSelection(point(selectionPoint_.x, selectionPoint_.y));
        selecting_ = true; SetCapture(window_); SetTimer(window_, selectionTimer, 50, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: if (selecting_) extendSelection(GET_X_LPARAM(b), GET_Y_LPARAM(b)); return 0;
    case WM_LBUTTONUP: if (selecting_) { extendSelection(GET_X_LPARAM(b), GET_Y_LPARAM(b)); stopSelection(); } return 0;
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
        // ConPTY expects DEL for Backspace and BS for Ctrl+Backspace.
        wchar_t c = static_cast<wchar_t>(a);
        if (c == L'\b') c = L'\x7f';
        else if (c == L'\x7f') c = L'\b';
        if (c >= 0xd800 && c <= 0xdbff) { highSurrogate_ = c; return 0; }
        std::wstring text; if (highSurrogate_) { text += highSurrogate_; highSurrogate_ = 0; } text += c;
        send((m == WM_SYSCHAR ? "\x1b" : "") + utf8(text)); return 0;
    }
    case WM_IME_SETCONTEXT: case WM_INPUTLANGCHANGE: case WM_IME_NOTIFY:
    case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_IME_COMPOSITION:
        return ime_.handle(window_, m, a, b, [this] { imePosition(); },
            [this](std::wstring text) { send(utf8(text)); });
    default: break;
    }
    return DefWindowProcW(window_, m, a, b);
}
}
