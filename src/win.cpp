#include "win.h"
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <cerrno>
#include <memory>
#include <process.h>
#include <stdexcept>
#include <system_error>

namespace poppy {
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
std::wstring quoteArg(std::wstring_view arg) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') ++slashes;
        else { result.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0; if (c == L'"') result += L'\\'; result += c; }
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
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
    std::wstring command; for (const auto& arg : args) { if (!command.empty()) command += L' '; command += quoteArg(arg); }
    PROCESS_INFORMATION pi{};
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { result.error = winError(); DeleteProcThreadAttributeList(attributes); return result; }
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &si.StartupInfo, &pi);
    const DWORD error = ok ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!ok) { result.error = winError(error); return result; }
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.value, process.value)) { result.error = winError(); TerminateProcess(process.value, 1); return result; }
    ResumeThread(thread.value); childOutput.reset(); input.reset();
    const ULONGLONG start = GetTickCount64();
    char buffer[8192];
    for (;;) {
        if (cancelled && *cancelled) { result.error = L"Git operation cancelled."; TerminateJobObject(job.value, 1); break; }
        DWORD available = 0;
        if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available) {
            DWORD count = 0;
            if (!ReadFile(output.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) || !count) break;
            result.output.append(buffer, count);
            if (result.output.size() > 4 * 1024 * 1024) { result.error = L"Git output exceeds the 4 MiB limit."; TerminateJobObject(job.value, 1); break; }
        } else if (WaitForSingleObject(process.value, 15) == WAIT_OBJECT_0) {
            // The process may have written its final bytes during the wait.
            DWORD remaining = 0;
            if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &remaining, nullptr) || !remaining) break;
        }
        if (GetTickCount64() - start > timeoutMs) { result.error = L"Git operation timed out."; TerminateJobObject(job.value, 1); break; }
    }
    if (result.error.empty()) {
        while (WaitForSingleObject(process.value, 15) != WAIT_OBJECT_0) {
            if (cancelled && *cancelled) { result.error = L"Git operation cancelled."; break; }
            if (GetTickCount64() - start >= timeoutMs) { result.error = L"Git operation timed out."; break; }
        }
        if (!result.error.empty()) TerminateJobObject(job.value, 1);
        else GetExitCodeProcess(process.value, &result.code);
    }
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
