#pragma once
#include "render.h"
#include "terminal_display.h"

namespace poppy {
class Terminal {
public:
    ~Terminal();
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void setDirectory(const fs::path& directory);
    bool receive();
    static void pump();
    void trackContainer(const std::string& id);
    void setCommand(std::vector<std::wstring> command) { command_ = std::move(command); }
    bool launch() { return start(); }
    bool cleanupUnconfirmed() const { return unconfirmed_ && WaitForSingleObject(unconfirmed_.value, 0) == WAIT_OBJECT_0; }
    void stop();
    void setEnvironment(std::vector<std::pair<std::wstring, std::wstring>> values) { environment_ = std::move(values); }
    std::function<void()> starting, ended, focused;
    HWND window() const { return window_; }
    bool running() const { return process_ && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT; }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    bool start();
    void send(std::string text, bool userInput = true);
    void paste();
    void copy();
    bool hasSelection() const { return anchor_ >= 0 && caret_ >= 0 && anchor_ != caret_; }
    void paint();
    void resize();
    void imePosition();
    int point(int x, int y) const;
    HWND window_ = nullptr;
    Renderer renderer_;
    int cellWidth_ = 8, cellHeight_ = 20, header_ = 30;
    fs::path directory_;
    std::vector<std::pair<std::wstring, std::wstring>> environment_;
    TerminalModel model_;
    TerminalDisplay display_;
    HPCON console_ = nullptr;
    Handle input_, output_, process_, job_;
    Handle stopRequest_, launching_, cleaned_, unconfirmed_;
    std::vector<std::wstring> command_;
    std::atomic<bool> stopping_{false};
    NativeThread reader_, writer_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::string received_;
    std::deque<std::string> writes_;
    size_t writeBytes_ = 0;
    bool notified_ = false, started_ = false, endedNotified_ = false;
    bool resizePending_ = false;
    int requestedColumns_ = 80, requestedRows_ = 24;
    std::wstring error_;
    size_t scroll_ = 0;
    int anchor_ = -1, caret_ = -1;
    bool selecting_ = false;
    wchar_t highSurrogate_ = 0;
};
}
