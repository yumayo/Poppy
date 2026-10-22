#pragma once
#include "win.h"

namespace poppy {
class Terminal {
public:
    ~Terminal();
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void setDirectory(const fs::path& directory);
    void receive();
    void stop();
    HWND window() const { return window_; }
    bool running() const { return process_ && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT; }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    bool start();
    void send(std::string text);
    void paste();
    void copy();
    void paint();
    void resize();
    void imePosition();
    int point(int x, int y) const;
    HWND window_ = nullptr;
    HFONT font_ = nullptr, uiFont_ = nullptr;
    int cellWidth_ = 8, cellHeight_ = 20, header_ = 30;
    fs::path directory_;
    TerminalModel model_;
    HPCON console_ = nullptr;
    Handle input_, output_, process_, job_;
    std::atomic<bool> stopping_{false};
    NativeThread reader_, writer_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::string received_;
    std::deque<std::string> writes_;
    size_t writeBytes_ = 0;
    bool notified_ = false, started_ = false;
    bool resizePending_ = false;
    int requestedColumns_ = 80, requestedRows_ = 24;
    std::wstring error_;
    size_t scroll_ = 0;
    int anchor_ = -1, caret_ = -1;
    bool selecting_ = false;
    wchar_t highSurrogate_ = 0;
};
}
