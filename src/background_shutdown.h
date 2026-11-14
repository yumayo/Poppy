#pragma once
#include "shutdown_progress.h"
#include "win.h"
#include <atomic>

namespace poppy {
// Runs a shutdown sequence while the message loop keeps running. The sequence
// runs on a fiber of the UI thread, so the steps between waits keep their UI
// thread affinity. Each wait() runs its task on a worker thread and suspends
// the fiber; when the task ends, `message` is posted to `notify` with `id` as
// WPARAM, and the owner calls resume() to continue to the next wait.
class BackgroundShutdown final : public ShutdownProgress {
public:
    BackgroundShutdown(HWND notify, UINT message, WPARAM id, std::function<void(ShutdownProgress&)> sequence);
    // Completes a started sequence synchronously so its stack is not abandoned.
    ~BackgroundShutdown() override;
    BackgroundShutdown(const BackgroundShutdown&) = delete;
    BackgroundShutdown& operator=(const BackgroundShutdown&) = delete;
    // Runs until the first wait or the end of the sequence. Call on the UI thread.
    void start();
    // Continues after the pending task's notification. Spurious calls are ignored.
    void resume();
    // Runs the remaining steps now. Waits use `progress` when given.
    void finish(ShutdownProgress* progress);
    bool done() const { return done_; }
    void detail(std::wstring) override {}
    void wait(const std::function<void()>& task) override;
private:
    static void WINAPI entry(void* parameter);
    void enter();
    void run();
    HWND notify_;
    UINT message_;
    WPARAM id_;
    std::function<void(ShutdownProgress&)> sequence_;
    void* fiber_ = nullptr;
    void* caller_ = nullptr;
    // The running wait() task, set only while the fiber is suspended on it.
    HANDLE task_ = nullptr;
    std::atomic<bool> taskDone_{false};
    ShutdownProgress* progress_ = nullptr;
    bool synchronous_ = false, started_ = false, done_ = false;
};
}
