#include "win.h"
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <cerrno>
#include <memory>
#include <imm.h>
#include <process.h>
#include <stdexcept>
#include <system_error>

namespace poppy {
fs::path applicationDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&applicationDirectory), &module))
        throw std::runtime_error("Cannot locate the Poppy application module");
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("Cannot locate the Poppy release directory");
    path.resize(count);
    return fs::path(path).parent_path();
}
bool ProcessJob::create() {
    stop();
    job_.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job_ && SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return true;
    const auto error = GetLastError(); job_.reset(); SetLastError(error); return false;
}
void ProcessJob::stop(DWORD exitCode) {
    if (!job_) return;
    TerminateJobObject(job_.value, exitCode);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    for (;;) {
        if (QueryInformationJobObject(job_.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) && !accounting.ActiveProcesses) break;
        TerminateJobObject(job_.value, exitCode); Sleep(10);
    }
    job_.reset();
}
void ImeComposition::update(HWND window, LPARAM flags) {
    if (!flags) { clear(); return; }
    active_ = true;
    if (flags & GCS_RESULTSTR) text_ = {};
    if (!(flags & (GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS))) return;
    const auto context = ImmGetContext(window); if (!context) return;
    if (flags & GCS_COMPSTR) {
        const auto bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
        std::wstring text(bytes > 0 ? static_cast<size_t>(bytes) / sizeof(wchar_t) : 0, L'\0');
        if (bytes > 0) ImmGetCompositionStringW(context, GCS_COMPSTR, text.data(), bytes);
        text_.text.assign(text.begin(), text.end());
    }
    const auto bytes = ImmGetCompositionStringW(context, GCS_COMPATTR, nullptr, 0);
    text_.attributes.assign(bytes > 0 ? static_cast<size_t>(bytes) : 0, 0);
    if (bytes > 0) ImmGetCompositionStringW(context, GCS_COMPATTR, text_.attributes.data(), bytes);
    const auto cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
    text_.cursor = cursor >= 0 ? std::min(static_cast<size_t>(cursor), text_.text.size()) : text_.text.size();
    ImmReleaseContext(window, context);
}
void ImeComposition::cancel(HWND window) {
    const bool composing = active_;
    clear();
    if (!composing || !window) return;
    const auto context = ImmGetContext(window);
    if (context) { ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0); ImmReleaseContext(window, context); }
    InvalidateRect(window, nullptr, FALSE);
}
LRESULT ImeComposition::handle(HWND window, UINT message, WPARAM a, LPARAM b,
                             const std::function<void()>& position,
                             const std::function<void(std::wstring)>& commit) {
    switch (message) {
    case WM_IME_SETCONTEXT: case WM_INPUTLANGCHANGE: {
        if (message == WM_IME_SETCONTEXT) b &= ~ISC_SHOWUICOMPOSITIONWINDOW;
        const auto result = DefWindowProcW(window, message, a, b);
        if (message == WM_INPUTLANGCHANGE || a) position();
        return result;
    }
    case WM_IME_NOTIFY:
        if (a == IMN_OPENCANDIDATE || a == IMN_CHANGECANDIDATE) position();
        return DefWindowProcW(window, message, a, b);
    case WM_IME_STARTCOMPOSITION: start(); break;
    case WM_IME_ENDCOMPOSITION: clear(); break;
    case WM_IME_COMPOSITION: {
        update(window, b);
        std::wstring result;
        if (b & GCS_RESULTSTR) {
            const auto context = ImmGetContext(window);
            const auto bytes = context ? ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0) : 0;
            if (bytes > 0) {
                result.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
                ImmGetCompositionStringW(context, GCS_RESULTSTR, result.data(), bytes);
            }
            if (context) ImmReleaseContext(window, context);
        }
        if (!result.empty()) commit(std::move(result));
        break;
    }
    default: return DefWindowProcW(window, message, a, b);
    }
    position(); InvalidateRect(window, nullptr, FALSE); return 0;
}
void positionImeCandidate(HWND window, HIMC context, POINT point, int height) {
    if (!context || GetFocus() != window) return;
    RECT area{}; GetClientRect(window, &area);
    POINT screenPoint = point; ClientToScreen(window, &screenPoint);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST), &monitor)) {
        area = monitor.rcWork;
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&area), 2);
    }
    // Exclude the input row across the monitor's work area so a tall list
    // flips above the text instead of moving alongside it. Let the IME choose
    // the vertical position using the candidate window's actual size.
    CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = {point.x, point.y + height};
    candidate.rcArea = {area.left, point.y, area.right, point.y + height};
    ImmSetCandidateWindow(context, &candidate);
}
void positionIme(HWND window, POINT point, int height, const LOGFONTW& font) {
    // Windows share the thread's default input context. Background output must
    // never replace the composition/candidate position of the focused control.
    if (!window || GetFocus() != window) return;
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(GetCurrentThreadId(), &info) || info.hwndCaret != window ||
        info.rcCaret.bottom - info.rcCaret.top != height)
        CreateCaret(window, nullptr, 2, height);
    // Keep the native caret hidden: Direct2D draws the visible cursor. Retain
    // it across moves so IME/accessibility can always query an insertion point.
    SetCaretPos(point.x, point.y);
    const auto context = ImmGetContext(window); if (!context) return;
    LOGFONTW current{};
    // Avoid resetting an active composition when the font already matches.
    if (!ImmGetCompositionFontW(context, &current) || std::memcmp(&current, &font, sizeof(font)) != 0) {
        auto updated = font;
        ImmSetCompositionFontW(context, &updated);
    }
    COMPOSITIONFORM form{}; form.dwStyle = CFS_POINT; form.ptCurrentPos = point;
    ImmSetCompositionWindow(context, &form);
    positionImeCandidate(window, context, point, height);
    ImmReleaseContext(window, context);
}
fs::path relocatePath(const fs::path& path, const fs::path& source, const fs::path& destination) {
    const auto normalized = path.lexically_normal().make_preferred();
    const auto from = source.lexically_normal().make_preferred();
    const auto key = pathKey(normalized), prefix = pathKey(from);
    if (key == prefix) return destination;
    if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0 && key[prefix.size()] == L'\\')
        return fs::path(destination.native() + normalized.native().substr(from.native().size()));
    return path;
}
unsigned __stdcall NativeThread::entry(void* argument) {
    std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(argument));
    (*task)();
    return 0;
}
void NativeThread::start(std::function<void()> task) {
    if (joinable()) throw std::logic_error("Thread is already running.");
    auto argument = std::make_unique<std::function<void()>>(std::move(task));
    const uintptr_t thread = _beginthreadex(nullptr, 0, entry, argument.get(), 0, nullptr);
    if (!thread) throw std::system_error(errno, std::generic_category(), "_beginthreadex");
    argument.release(); handle_.reset(reinterpret_cast<HANDLE>(thread));
}
void NativeThread::join() {
    if (!joinable()) return;
    WaitForSingleObject(handle_.value, INFINITE); handle_.reset();
}
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length); return result;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr); return result;
}
std::wstring winError(DWORD error) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = buffer ? buffer : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}
std::wstring pathKey(const fs::path& path) {
    auto result = path.lexically_normal().make_preferred().native();
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}
bool copyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!data) { CloseClipboard(); return false; }
    void* buffer = GlobalLock(data);
    if (!buffer) { GlobalFree(data); CloseClipboard(); return false; }
    memcpy(buffer, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(data);
    EmptyClipboard(); const bool success = SetClipboardData(CF_UNICODETEXT, data) != nullptr;
    if (!success) GlobalFree(data);
    CloseClipboard(); return success;
}
std::wstring clipboardText(HWND owner) {
    if (!OpenClipboard(owner)) return {};
    const auto data = GetClipboardData(CF_UNICODETEXT);
    const auto buffer = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
    std::wstring result;
    if (buffer) {
        const size_t capacity = std::min<size_t>(GlobalSize(data) / sizeof(wchar_t), 256 * 1024);
        size_t count = 0; while (count < capacity && buffer[count]) ++count;
        result.assign(buffer, count); GlobalUnlock(data);
    }
    CloseClipboard(); return result;
}
ProcessResult runProcess(const std::vector<std::wstring>& args, const fs::path& directory, DWORD timeoutMs, const std::atomic<bool>* cancelled) {
    ProcessResult result;
    if (cancelled && *cancelled) { result.error = L"Git operation cancelled."; return result; }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) { result.error = winError(); return result; }
    Handle output(read), childOutput(write);
    SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0);
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    if (!input) { result.error = winError(); return result; }
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) { result.error = winError(); return result; }
    HANDLE inherited[] = {input.value, childOutput.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) { result.error = winError(); DeleteProcThreadAttributeList(attributes); return result; }
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); si.lpAttributeList = attributes;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES; si.StartupInfo.hStdInput = input.value; si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = childOutput.value;
    auto command = processCommandLine(args);
    PROCESS_INFORMATION pi{};
    ProcessJob job;
    if (!job.create()) { result.error = winError(); DeleteProcThreadAttributeList(attributes); return result; }
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &si.StartupInfo, &pi);
    const DWORD error = ok ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!ok) { result.error = winError(error); return result; }
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.handle(), process.value)) { result.error = winError(); TerminateProcess(process.value, 1); WaitForSingleObject(process.value, INFINITE); return result; }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) { result.error = winError(); return result; }
    childOutput.reset(); input.reset();
    const ULONGLONG start = GetTickCount64();
    char buffer[8192];
    for (;;) {
        if (cancelled && *cancelled) { result.error = L"Git operation cancelled."; break; }
        DWORD available = 0;
        if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available) {
            DWORD count = 0;
            if (!ReadFile(output.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) || !count) break;
            result.output.append(buffer, count);
            if (result.output.size() > 4 * 1024 * 1024) { result.error = L"Git output exceeds the 4 MiB limit."; break; }
        } else if (WaitForSingleObject(process.value, 15) == WAIT_OBJECT_0) {
            // The process may have written its final bytes during the wait.
            DWORD remaining = 0;
            if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &remaining, nullptr) || !remaining) break;
        }
        if (timeoutMs != INFINITE && GetTickCount64() - start > timeoutMs) { result.error = L"Git operation timed out."; break; }
    }
    if (result.error.empty()) {
        while (WaitForSingleObject(process.value, 15) != WAIT_OBJECT_0) {
            if (cancelled && *cancelled) { result.error = L"Git operation cancelled."; break; }
            if (timeoutMs != INFINITE && GetTickCount64() - start >= timeoutMs) { result.error = L"Git operation timed out."; break; }
        }
        if (result.error.empty()) GetExitCodeProcess(process.value, &result.code);
    }
    job.stop();
    if (result.code != 0 && result.error.empty()) result.error = wide(result.output);
    return result;
}
Worker::Worker(HWND window) : window_(window), thread_([this] {
    for (;;) {
        std::function<void()> task;
        { std::unique_lock<std::mutex> lock(mutex_); condition_.wait(lock, [&] { return stopping_ || !jobs_.empty(); }); if (stopping_) return; task = std::move(jobs_.front().second); jobs_.pop_front(); }
        try { task(); } catch (const std::exception&) { /* Every job reports ordinary I/O errors itself. */ }
    }
}) {}
Worker::~Worker() { stop(); }
void Worker::submit(std::wstring key, std::function<void()> task) {
    { std::lock_guard<std::mutex> lock(mutex_); if (stopping_) return;
      jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const auto& job) { return job.first == key; }), jobs_.end());
      jobs_.emplace_back(std::move(key), std::move(task)); }
    condition_.notify_one();
}
void Worker::complete(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(mutex_); if (stopping_) return;
    completions_.push_back(std::move(task)); PostMessageW(window_, wmWork, 0, 0);
}
void Worker::dispatch() {
    std::deque<std::function<void()>> tasks;
    { std::lock_guard<std::mutex> lock(mutex_); tasks.swap(completions_); }
    for (auto& task : tasks) task();
}
void Worker::cancelPending() { std::lock_guard<std::mutex> lock(mutex_); jobs_.clear(); completions_.clear(); }
void Worker::drain() {
    Handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    submit(L"drain", [event = done.value] { SetEvent(event); });
    WaitForSingleObject(done.value, INFINITE);
}
void Worker::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; jobs_.clear(); completions_.clear(); }
    condition_.notify_one();
    if (thread_.joinable()) {
        while (WaitForSingleObject(thread_.native_handle(), 0) == WAIT_TIMEOUT) { CancelSynchronousIo(thread_.native_handle()); WaitForSingleObject(thread_.native_handle(), 10); }
        thread_.join();
    }
}
}
