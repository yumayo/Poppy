#pragma once
#include "scrollbars.h"
#include "terminal_display.h"
#include "terminal_host.h"

namespace poppy {
class ShutdownDialog;
class Terminal {
public:
    struct LaunchOptions : TerminalHostLaunch {
        // Readiness runs on the UI thread; failure cleanup runs on the worker.
        std::function<void()> ready, failed;
    };
    // Snapshot UI state first, then perform preparation on the launch thread.
    using StartupTask = std::function<LaunchOptions()>;
    ~Terminal();
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void setDirectory(const fs::path& directory);
    bool receive();
    static void pump();
    void setCommand(std::vector<std::wstring> command) { command_ = std::move(command); }
    void setDisplayDirectory(std::wstring label) { displayDirectory_ = std::move(label); }
    // Returns whether the launch request was accepted. Completion is asynchronous.
    bool launch() { return start(); }
    void setSessionIdentity(std::string id) { sessionId_ = std::move(id); }
    void reconnect() { start(true); }
    void detach(ShutdownDialog* progress = nullptr);
    void stop(ShutdownDialog* progress = nullptr);
    void setEnvironment(std::vector<std::pair<std::wstring, std::wstring>> values) { environment_ = std::move(values); }
    std::function<StartupTask()> starting;
    std::function<void()> ended, focused, stopping;
    std::function<void()> connected;
    std::function<void(const TerminalHostUpdate&)> hostStatus;
    std::function<void()> titleChanged;
    std::function<void(const std::string&)> notificationError;
    std::function<void(const std::wstring&)> launchFailed;
    std::function<void(const std::wstring&)> shutdownFailed;
    const std::string& title() const { return model_.title(); }
    HWND window() const { return window_; }
    bool launching() const { return launching_; }
    bool running() const { return !launching_ && running_; }
    bool relayRunning() const { return relayRunning_; }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    bool start(bool reconnect = false);
    void readHost(HWND target);
    void completeStart();
    void stopProcesses();
    void send(std::string text, bool userInput = true);
    void paste();
    void copy();
    bool hasSelection() const { return display_.hasSelection(); }
    void paint();
    void resize();
    void scrollbars();
    void scroll(int offset);
    void extendSelection(int x, int y);
    void scrollSelection();
    void stopSelection();
    void imePosition();
    int point(int x, int y) const;
    std::string linkAt(int x, int y) const;
    HWND window_ = nullptr;
    Renderer renderer_{D2D1_PRESENT_OPTIONS_IMMEDIATELY};
    ImeComposition ime_;
    Scrollbars scrollbars_;
    int cellWidth_ = 8, cellHeight_ = 20;
    fs::path directory_;
    std::wstring displayDirectory_;
    std::vector<std::pair<std::wstring, std::wstring>> environment_;
    TerminalModel model_;
    TerminalDisplay display_;
    TerminalHostClient host_;
    std::string sessionId_;
    std::vector<std::wstring> command_;
    std::atomic<bool> stopping_{false};
    NativeThread launcher_, reader_;
    bool launching_ = false;
    std::atomic<bool> launchFinished_{false};
    std::function<void()> launchReady_;
    std::wstring launchError_;
    std::wstring shutdownError_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<TerminalHostUpdate> received_;
    size_t receivedBytes_ = 0;
    std::deque<std::string> writes_;
    size_t writeBytes_ = 0;
    bool notified_ = false, started_ = false, endedNotified_ = false, continuationPosted_ = false;
    bool resizePending_ = false;
    bool hostFound_ = false;
    bool sizeKnown_ = false, restored_ = false;
    std::atomic<bool> preserve_{false};
    std::atomic<bool> running_{false}, relayRunning_{false};
    int requestedColumns_ = 80, requestedRows_ = 24;
    std::wstring error_;
    size_t scroll_ = 0;
    bool selecting_ = false;
    POINT selectionPoint_{};
    POINT linkPoint_{};
    std::string pressedLink_;
    static constexpr UINT_PTR selectionTimer = 0x5058;
    wchar_t highSurrogate_ = 0;
};
}
