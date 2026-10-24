#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include "terminal_model.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <utility>

namespace poppy {
namespace fs = std::filesystem;
constexpr UINT wmWork = WM_APP + 1, wmTerminal = WM_APP + 2;
constexpr COLORREF panel = RGB(28, 32, 39), border = RGB(46, 53, 64), muted = RGB(132, 145, 164), accent = RGB(151, 195, 121);
constexpr COLORREF scrollbarThumb = RGB(63, 73, 88), scrollbarHover = RGB(83, 97, 116);
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : value(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value(std::exchange(h.value, nullptr)) {}
    Handle& operator=(Handle&& h) noexcept { if (this != &h) reset(std::exchange(h.value, nullptr)); return *this; }
    void reset(HANDLE v = nullptr) { if (value) CloseHandle(value); value = v == INVALID_HANDLE_VALUE ? nullptr : v; }
    explicit operator bool() const { return value != nullptr; }
};

// A real Windows thread HANDLE is required by CancelSynchronousIo. MinGW's
// std::thread::native_handle() is a pthread_t when using the POSIX runtime.
class NativeThread {
public:
    NativeThread() = default;
    explicit NativeThread(std::function<void()> task) { start(std::move(task)); }
    ~NativeThread() { join(); }
    NativeThread(const NativeThread&) = delete;
    NativeThread& operator=(const NativeThread&) = delete;
    void start(std::function<void()> task);
    bool joinable() const { return static_cast<bool>(handle_); }
    HANDLE native_handle() const { return handle_.value; }
    void join();
private:
    static unsigned __stdcall entry(void* task);
    Handle handle_;
};
std::wstring wide(std::string_view utf8);
std::string utf8(std::wstring_view text);
std::wstring winError(DWORD error = GetLastError());
std::wstring quoteArg(std::wstring_view arg);
std::wstring pathKey(const fs::path& path);
bool copyText(HWND owner, const std::wstring& text);
std::wstring clipboardText(HWND owner);

struct ProcessResult { DWORD code = 1; std::string output; std::wstring error; };
ProcessResult runProcess(const std::vector<std::wstring>& args, const fs::path& directory, DWORD timeoutMs = 5000, const std::atomic<bool>* cancelled = nullptr);

// One worker, coalesced jobs, and UI-thread completions. UI objects never cross threads.
class Worker {
public:
    explicit Worker(HWND window);
    ~Worker();
    void submit(std::wstring key, std::function<void()> task);
    void complete(std::function<void()> task);
    void dispatch();
    void cancelPending();
    void drain();
    void stop();
    const std::atomic<bool>* cancellation() const { return &stopping_; }
private:
    HWND window_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::pair<std::wstring, std::function<void()>>> jobs_;
    std::deque<std::function<void()>> completions_;
    std::atomic<bool> stopping_{false};
    NativeThread thread_;
};
}
