#include "terminal.h"
#include <imm.h>
#include <algorithm>
#include <map>
#include <stdexcept>

namespace poppy {
namespace {
std::vector<Terminal*> liveTerminals;
size_t nextTerminal = 0;
}
Terminal::~Terminal() {
    stop();
    liveTerminals.erase(std::remove(liveTerminals.begin(), liveTerminals.end(), this), liveTerminals.end());
}
void Terminal::trackContainer(const std::string& id) {
    const auto prefix = L"Local\\Poppy." + wide(id);
    stopRequest_.reset(CreateEventW(nullptr, TRUE, FALSE, (prefix+L".stop").c_str()));
    launching_.reset(CreateEventW(nullptr, TRUE, FALSE, (prefix+L".launching").c_str()));
    cleaned_.reset(CreateEventW(nullptr, TRUE, FALSE, (prefix+L".cleaned").c_str()));
    unconfirmed_.reset(CreateEventW(nullptr, TRUE, FALSE, (prefix+L".unconfirmed").c_str()));
    if (!stopRequest_ || !launching_ || !cleaned_ || !unconfirmed_) throw std::runtime_error("Cannot create container lifecycle events");
}

void Terminal::create(HWND parent, HINSTANCE instance) {
    liveTerminals.push_back(this);
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyTerminal"; cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM); RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"AI terminal", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, instance, this);
    model_.reply = [this](std::string response) { send(std::move(response)); };
    setDpi(GetDpiForWindow(parent));
}
void Terminal::setDpi(UINT dpi) {
    renderer_.setDpi(dpi);
    cellWidth_ = renderer_.cellWidth(); cellHeight_ = renderer_.textHeight() + MulDiv(2, dpi, 96); header_ = MulDiv(30, dpi, 96);
    resize(); InvalidateRect(window_, nullptr, FALSE);
}
void Terminal::setDirectory(const fs::path& directory) {
    if (!started_) { directory_ = directory; error_.clear(); InvalidateRect(window_, nullptr, FALSE); }
}
bool Terminal::start() {
    if (running()) return true;
    stop();
    std::error_code directoryError;
    if (directory_.empty() || !fs::is_directory(directory_, directoryError)) { error_ = L"Project folder is missing. Restore the folder before starting a shell."; return false; }
    if (starting) starting();
    error_.clear(); model_ = TerminalModel(model_.columns(), model_.rows()); model_.reply = [this](std::string s) { send(std::move(s)); };
    scroll_ = 0; anchor_ = caret_ = -1;
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, nullptr, 0)) { error_ = winError(); return false; }
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
    if (!command_.empty()) { command.clear(); for (const auto& arg : command_) { if (!command.empty()) command += L' '; command += quoteArg(arg); } }
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
    if (!environment_.empty()) {
        auto& wslenv = inherited[L"WSLENV"];
        for (const auto name : {L"POPPY_EVENTS_URL", L"POPPY_SESSION_ID"}) {
            bool found = false;
            for (size_t p = 0; p < wslenv.size();) { const auto end = wslenv.find(L':', p); const auto token = wslenv.substr(p, end == std::wstring::npos ? end : end-p); if (token.substr(0,token.find(L'/')) == name) found = true; if (end == std::wstring::npos) break; p = end+1; }
            if (!found) { if (!wslenv.empty()) wslenv += L':'; wslenv += name; }
        }
    }
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
    bool unknownCleanup = cleanupUnconfirmed();
    if (launching_ && WaitForSingleObject(launching_.value, 0) == WAIT_OBJECT_0) {
        SetEvent(stopRequest_.value);
        unknownCleanup = WaitForSingleObject(cleaned_.value, 15000) != WAIT_OBJECT_0;
    }
    if (unknownCleanup && window_) MessageBoxW(GetAncestor(window_, GA_ROOT), L"Container cleanup could not be confirmed. The launch profile is retained under .poppy-runtime. Check the host launcher's output before removing its resources.", L"Poppy — cleanup unconfirmed", MB_OK | MB_ICONWARNING);
    stopRequest_.reset(); launching_.reset(); cleaned_.reset(); unconfirmed_.reset();
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
}
void Terminal::send(std::string text) {
    if (!running() || text.empty()) return;
    { std::lock_guard<std::mutex> lock(mutex_); if (writeBytes_ + text.size() > 1024 * 1024) { MessageBeep(MB_ICONWARNING); return; } writeBytes_ += text.size(); writes_.push_back(std::move(text)); }
    condition_.notify_all(); scroll_ = 0; anchor_ = caret_ = -1; InvalidateRect(window_, nullptr, FALSE);
}
void Terminal::receive() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t count = std::min<size_t>(received_.size(), 8192);
        text.assign(received_.data(), count); received_.erase(0, count);
        // Keep notifications latched until the next tick, even if the queue is
        // temporarily empty. This gives paints and input time during a flood.
        notified_ = count != 0;
    }
    condition_.notify_all();
    if (text.empty()) { if (started_ && !running() && ended) ended(); if (IsWindowVisible(window_)) InvalidateRect(window_, nullptr, FALSE); return; }
    model_.feed(text); scroll_ = std::min(scroll_, model_.historySize());
    if (!running() && ended) ended();
    if (IsWindowVisible(window_)) { imePosition(); InvalidateRect(window_, nullptr, FALSE); }
}
void Terminal::pump() {
    static LONGLONG cycleStart = 0, spent = 0;
    LARGE_INTEGER now{}, frequency{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    if (now.QuadPart - cycleStart >= frequency.QuadPart / 60) { cycleStart = now.QuadPart; spent = 0; }
    HWND root = nullptr;
    for (size_t visited = 0; visited < liveTerminals.size() && spent < frequency.QuadPart / 200; ++visited) {
        if (nextTerminal >= liveTerminals.size()) nextTerminal = 0;
        auto terminal = liveTerminals[nextTerminal++];
        if (!terminal->window_) continue;
        root = GetAncestor(terminal->window_, GA_ROOT);
        LARGE_INTEGER begin{}, end{}; QueryPerformanceCounter(&begin);
        terminal->receive(); QueryPerformanceCounter(&end); spent += end.QuadPart - begin.QuadPart;
    }
    bool pending = false;
    for (const auto terminal : liveTerminals) { std::lock_guard<std::mutex> lock(terminal->mutex_); pending |= !terminal->received_.empty() || terminal->notified_; if (terminal->window_) root = GetAncestor(terminal->window_, GA_ROOT); }
    if (root) { if (pending) SetTimer(root, 77, 16, nullptr); else KillTimer(root, 77); }
}
void Terminal::resize() {
    if (!window_) return;
    RECT rect{}; GetClientRect(window_, &rect);
    if (rect.right < cellWidth_ * 2 || rect.bottom <= header_) return;
    const int columns = std::clamp((static_cast<int>(rect.right) - 12) / cellWidth_, 2, 500);
    const int rows = std::clamp((static_cast<int>(rect.bottom) - header_ - 8) / cellHeight_, 1, 200);
    if (columns == model_.columns() && rows == model_.rows()) return;
    model_.resize(columns, rows); scroll_ = 0; anchor_ = caret_ = -1;
    if (console_) {
        { std::lock_guard<std::mutex> lock(mutex_); requestedColumns_ = columns; requestedRows_ = rows; resizePending_ = true; }
        condition_.notify_all();
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void Terminal::imePosition() {
    const auto context = ImmGetContext(window_); if (!context) return;
    COMPOSITIONFORM form{}; form.dwStyle = CFS_POINT; form.ptCurrentPos = {6 + model_.cursorX() * cellWidth_, header_ + 4 + model_.cursorY() * cellHeight_};
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
    return std::clamp((y - header_ - 4) / cellHeight_, 0, model_.rows() - 1) * model_.columns() + std::clamp((x - 6) / cellWidth_, 0, model_.columns() - 1);
}
void Terminal::copy() {
    if (anchor_ < 0 || caret_ < 0) return;
    const int begin = std::min(anchor_, caret_), end = std::max(anchor_, caret_); std::string result;
    for (int y = begin / model_.columns(); y <= end / model_.columns(); ++y) {
        const auto& line = model_.row(y, scroll_); std::string part;
        const int first = y == begin / model_.columns() ? begin % model_.columns() : 0;
        const int last = y == end / model_.columns() ? end % model_.columns() + 1 : model_.columns();
        for (int x = first; x < last && x < static_cast<int>(line.size()); ++x) if (!line[x].continuation) appendUtf8(part, line[x].ch);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        result += part;
        if (y != end / model_.columns()) result += "\r\n";
    }
    copyText(window_, wide(result));
}
void Terminal::paint() {
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    auto& dc = renderer_; dc.font(Renderer::Font::Ui);
    RECT rect{}; GetClientRect(window_, &rect);
    dc.fill({0, 0, rect.right, header_}, panel);
    dc.label({12, 0, rect.right - 12, header_}, L"TERMINAL  ·  " + (directory_.empty() ? std::wstring(L"PowerShell") : directory_.native()), muted);
    dc.fill({0, header_ - 1, rect.right, header_}, border);
    if (!started_) {
        dc.fill({0, header_, rect.right, rect.bottom}, background);
        RECT text{20, header_ + 18, std::max<LONG>(20, rect.right - 20), rect.bottom};
        dc.label(text, error_.empty() ? L"Click to start PowerShell, then run your AI CLI.\nThe shell starts only when needed." : error_ + L"\nClick to retry.", muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        RECT clip{6, header_ + 4, rect.right - 6, rect.bottom};
        dc.fill({0, header_, 6, rect.bottom}, background); dc.fill({6, header_, rect.right, header_ + 4}, background);
        const int begin = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
        std::vector<Cell> visible;
        for (int y = 0; y < model_.rows(); ++y) {
            visible = model_.row(y, scroll_); visible.resize(model_.columns());
            if (begin >= 0) for (int x = 0; x < model_.columns(); ++x) { const int index = y * model_.columns() + x; if (index >= begin && index <= end) visible[x].bg = rgb(50,70,94); }
            const int sy = header_ + 4 + y * cellHeight_;
            dc.cells(6, sy, cellWidth_, cellHeight_, visible, model_.columns(), clip);
            dc.fill({6 + model_.columns() * cellWidth_, sy, rect.right, sy + cellHeight_}, background);
        }
        dc.fill({0, header_ + 4 + model_.rows() * cellHeight_, rect.right, rect.bottom}, background);
        if (!scroll_ && running() && model_.cursorVisible() && GetFocus() == window_) {
            const int x = 6 + model_.cursorX() * cellWidth_, y = header_ + 4 + model_.cursorY() * cellHeight_;
            dc.fill({x, y + cellHeight_ - 2, x + cellWidth_, y + cellHeight_}, accent);
        }
        if (!running()) dc.label({12, 0, rect.right - 12, header_}, L"SESSION ENDED  ·  Click header to restart", accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}
LRESULT CALLBACK Terminal::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<Terminal*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<Terminal*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT Terminal::handle(UINT m, WPARAM a, LPARAM b) {
    switch (m) {
    case WM_NCDESTROY: { const auto result = DefWindowProcW(window_, m, a, b); window_ = nullptr; return result; }
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: resize(); return 0;
    case wmTerminal: pump(); return 0;
    case WM_TIMER: if (a == 2) pump(); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window_);
        if (!started_ || (!running() && GET_Y_LPARAM(b) < header_)) { start(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        if (GET_Y_LPARAM(b) >= header_) { anchor_ = caret_ = point(GET_X_LPARAM(b), GET_Y_LPARAM(b)); selecting_ = true; SetCapture(window_); InvalidateRect(window_, nullptr, FALSE); } return 0;
    case WM_MOUSEMOVE: if (selecting_) { caret_ = point(GET_X_LPARAM(b), GET_Y_LPARAM(b)); InvalidateRect(window_, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: if (selecting_) { selecting_ = false; ReleaseCapture(); } return 0;
    case WM_CAPTURECHANGED: selecting_ = false; return 0;
    case WM_RBUTTONUP: if (!started_) start(); paste(); return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * 3;
        scroll_ = static_cast<size_t>(std::clamp<long long>(static_cast<long long>(scroll_) + delta, 0, static_cast<long long>(model_.historySize())));
        anchor_ = caret_ = -1; InvalidateRect(window_, nullptr, FALSE); return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (ctrl && shift && a == 'C') { copy(); return 0; }
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
        case VK_ESCAPE: send("\x1b"); return 0;
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
        const wchar_t c = static_cast<wchar_t>(a); if (c >= 0xd800 && c <= 0xdbff) { highSurrogate_ = c; return 0; }
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
