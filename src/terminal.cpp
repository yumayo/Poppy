#include "terminal.h"
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
    cellWidth_ = renderer_.cellWidth(); cellHeight_ = renderer_.textHeight() + MulDiv(2, dpi, 96); header_ = MulDiv(30, dpi, 96);
    scrollbars(); resize(); InvalidateRect(window_, nullptr, FALSE);
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
    scroll_ = 0; anchor_ = caret_ = -1;
    endedNotified_ = false;
    display_ = TerminalDisplay{}; display_.update(model_, scroll_, GetTickCount64(), false, true);
    scrollbars();
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
    const std::wstring shell = !command_.empty() ? command_.front() : std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command = quoteArg(shell) + L" -NoLogo -NoProfile -NoExit -Command \"[Console]::InputEncoding = [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()\"";
    if (!command_.empty()) command = processCommandLine(command_);
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
    }
    inherited[L"WSLENV"] = std::move(wslenv);
    std::vector<wchar_t> environment;
    for (const auto& value : inherited) { const auto entry = value.first + L"=" + value.second; environment.insert(environment.end(),entry.begin(),entry.end()); environment.push_back(0); }
    environment.push_back(0);
    const BOOL ok = CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, environment.data(), directory_.empty() ? nullptr : directory_.c_str(), &si.StartupInfo, &pi);
    const DWORD error = ok ? 0 : GetLastError(); DeleteProcThreadAttributeList(attributes);
    if (!ok) { error_ = winError(error); stop(); return false; }
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
    display_.update(model_, scroll_, GetTickCount64(), false, true);
    scrollbars();
}
void Terminal::send(std::string text, bool userInput) {
    if (!running() || text.empty()) return;
    { std::lock_guard<std::mutex> lock(mutex_); if (writeBytes_ + text.size() > 1024 * 1024) { MessageBeep(MB_ICONWARNING); return; } writeBytes_ += text.size(); writes_.push_back(std::move(text)); }
    condition_.notify_all();
    if (userInput) {
        display_.input(GetTickCount64());
        const bool selected = hasSelection();
        scroll_ = 0; anchor_ = caret_ = -1;
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
    model_.feed(text); scroll_ = std::min(scroll_, model_.historySize());
    display_.changed(GetTickCount64());
    return true;
}
void Terminal::pump() {
    static LONGLONG cycleStart = 0, spent = 0;
    LARGE_INTEGER now{}, frequency{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    if (now.QuadPart - cycleStart >= frequency.QuadPart / 60) { cycleStart = now.QuadPart; spent = 0; }
    HWND root = nullptr;
    // Take another round when time remains: one 8 KiB chunk per timer tick made
    // replaying a long conversation unnecessarily slow, even on an idle UI.
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
    bool pending = false;
    for (const auto terminal : liveTerminals) {
        if (!terminal->window_) continue;
        root = GetAncestor(terminal->window_, GA_ROOT);
        bool queued = false;
        { std::lock_guard<std::mutex> lock(terminal->mutex_); queued = !terminal->received_.empty(); }
        const bool finished = terminal->started_ && !terminal->running();
        if (finished && !terminal->endedNotified_) {
            terminal->endedNotified_ = true;
            terminal->display_.changed(GetTickCount64());
            if (terminal->ended) terminal->ended();
        }
        const int oldColumns = terminal->display_.columns(), oldRows = terminal->display_.rows();
        if (terminal->display_.update(terminal->model_, terminal->scroll_, GetTickCount64(), queued,
            finished && !queued && terminal->display_.pending())) {
            if (oldColumns != terminal->display_.columns() || oldRows != terminal->display_.rows())
                terminal->anchor_ = terminal->caret_ = -1;
            terminal->scrollbars();
            terminal->imePosition();
            if (IsWindowVisible(terminal->window_)) {
                InvalidateRect(terminal->window_, nullptr, FALSE);
                // Key repeat can keep WM_PAINT behind input in the queue.
                UpdateWindow(terminal->window_);
            }
        }
        // Keep the reader notification latched throughout a burst, including
        // the quiet period and sync timeout after the receive queue runs dry.
        std::lock_guard<std::mutex> lock(terminal->mutex_);
        terminal->notified_ = !terminal->received_.empty() || terminal->display_.pending();
        pending |= terminal->notified_;
    }
    if (pumpTimerRoot && (!pending || pumpTimerRoot != root)) { KillTimer(pumpTimerRoot, 77); pumpTimerRoot = nullptr; }
    // Keep the cadence when more input arrives; resetting an existing timer
    // would postpone the next echo frame on every keystroke.
    if (root && pending && !pumpTimerRoot && SetTimer(root, 77, 16, nullptr)) pumpTimerRoot = root;
}
void Terminal::resize() {
    if (!window_) return;
    const auto rect = Scrollbars::contentRect(window_);
    if (rect.right < cellWidth_ * 2 || rect.bottom <= header_) return;
    const int columns = std::clamp((static_cast<int>(rect.right) - 12) / cellWidth_, 2, 500);
    const int rows = std::clamp((static_cast<int>(rect.bottom) - header_ - 8) / cellHeight_, 1, 200);
    if (columns == model_.columns() && rows == model_.rows()) return;
    model_.resize(columns, rows); scroll_ = 0; anchor_ = caret_ = -1;
    scrollbars();
    display_.resized(GetTickCount64());
    if (console_) {
        { std::lock_guard<std::mutex> lock(mutex_); requestedColumns_ = columns; requestedRows_ = rows; resizePending_ = true; }
        condition_.notify_all();
    }
    else display_.update(model_, scroll_, GetTickCount64(), false, true);
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
    anchor_ = caret_ = -1;
    display_.update(model_, scroll_, GetTickCount64());
    scrollbars(); imePosition(); InvalidateRect(window_, nullptr, FALSE);
    // Present each wheel or thumb position while input continues to arrive.
    UpdateWindow(window_);
}
void Terminal::imePosition() {
    const auto context = ImmGetContext(window_); if (!context) return;
    COMPOSITIONFORM form{}; form.dwStyle = CFS_POINT; form.ptCurrentPos = {6 + display_.cursorX() * cellWidth_, header_ + 4 + display_.cursorY() * cellHeight_};
    ImmSetCompositionWindow(context, &form);
    CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS; candidate.ptCurrentPos = {form.ptCurrentPos.x, form.ptCurrentPos.y + cellHeight_}; ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(window_, context);
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
    if (y < header_ + 4) return 0;
    if (y >= header_ + 4 + rows * cellHeight_) return rows * columns;
    return ((y - header_ - 4) / cellHeight_) * columns + std::clamp((x - 6 + cellWidth_ / 2) / cellWidth_, 0, columns);
}
void Terminal::copy() {
    if (!hasSelection()) return;
    if (copyText(window_, wide(display_.selectedText(anchor_, caret_)))) {
        anchor_ = caret_ = -1;
        InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_);
    } else {
        MessageBeep(MB_ICONWARNING);
    }
}
void Terminal::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    auto& dc = renderer_; dc.font(Renderer::Font::Ui);
    const auto rect = Scrollbars::contentRect(window_);
    dc.fill({0, 0, rect.right, header_}, panel);
    dc.label({12, 0, rect.right - 12, header_}, L"TERMINAL  ·  " + (!displayDirectory_.empty() ? displayDirectory_ : directory_.empty() ? std::wstring(L"PowerShell") : directory_.native()), muted);
    dc.fill({0, header_ - 1, rect.right, header_}, border);
    if (!started_) {
        dc.fill({0, header_, rect.right, rect.bottom}, background);
        RECT text{20, header_ + 18, std::max<LONG>(20, rect.right - 20), rect.bottom};
        dc.label(text, error_.empty() ? !displayDirectory_.empty() ? L"Click to start the WSL shell.\nThe shell starts only when needed." : L"Click to start PowerShell, then run your AI CLI.\nThe shell starts only when needed." : error_ + L"\nClick to retry.", muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        RECT clip{6, header_ + 4, rect.right - 6, rect.bottom};
        dc.fill({0, header_, rect.right, rect.bottom}, background);
        const auto [begin, end] = display_.selectionRange(anchor_, caret_);
        std::vector<Cell> visible;
        for (int y = 0; y < display_.rows(); ++y) {
            visible = display_.row(y);
            if (begin < end) for (int x = 0; x < display_.columns(); ++x) { const int index = y * display_.columns() + x; if (index >= begin && index < end) visible[x].bg = rgb(50,70,94); }
            const int sy = header_ + 4 + y * cellHeight_;
            dc.cells(6, sy, cellWidth_, cellHeight_, visible, display_.columns(), clip);
        }
        if (running() && display_.cursorVisible() && GetFocus() == window_) {
            const int x = 6 + display_.cursorX() * cellWidth_, y = header_ + 4 + display_.cursorY() * cellHeight_;
            if (x + cellWidth_ <= clip.right && y + cellHeight_ <= clip.bottom)
                dc.fill({x, y + cellHeight_ - 2, x + cellWidth_, y + cellHeight_}, accent);
        }
        if (!running()) dc.label({12, 0, rect.right - 12, header_}, L"SESSION ENDED  ·  Click header to restart", accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
    case wmTerminal: pump(); return 0;
    case WM_TIMER: if (a == 2) pump(); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_COPY: copy(); return 0;
    case WM_SETFOCUS: if (focused) focused(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window_);
        if (!started_ || (!running() && GET_Y_LPARAM(b) < header_)) { start(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        if (GET_Y_LPARAM(b) >= header_) { anchor_ = caret_ = point(GET_X_LPARAM(b), GET_Y_LPARAM(b)); selecting_ = true; SetCapture(window_); InvalidateRect(window_, nullptr, FALSE); } return 0;
    case WM_MOUSEMOVE: if (selecting_) { caret_ = point(GET_X_LPARAM(b), GET_Y_LPARAM(b)); InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_); } return 0;
    case WM_LBUTTONUP: if (selecting_) { caret_ = point(GET_X_LPARAM(b), GET_Y_LPARAM(b)); selecting_ = false; ReleaseCapture(); InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_); } return 0;
    case WM_CAPTURECHANGED: selecting_ = false; return 0;
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
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
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
            if (hasSelection()) { anchor_ = caret_ = -1; InvalidateRect(window_, nullptr, FALSE); }
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
    case WM_IME_STARTCOMPOSITION: imePosition(); break;
    case WM_IME_COMPOSITION:
        if (b & GCS_RESULTSTR) {
            const auto context = ImmGetContext(window_);
            const LONG bytes = context ? ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0) : 0;
            if (bytes > 0) { std::wstring text(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0'); ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes); send(utf8(text)); }
            if (context) ImmReleaseContext(window_, context);
            return 0;
        } break;
    default: break;
    }
    return DefWindowProcW(window_, m, a, b);
}
}
