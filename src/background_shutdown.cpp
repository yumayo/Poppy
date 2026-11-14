#include "background_shutdown.h"
#include <exception>

namespace poppy {
BackgroundShutdown::BackgroundShutdown(HWND notify, UINT message, WPARAM id, std::function<void(ShutdownProgress&)> sequence)
    : notify_(notify), message_(message), id_(id), sequence_(std::move(sequence)) {
    // The UI thread stays a fiber afterwards. Converting it back while another
    // sequence is suspended would strand that sequence's stack.
    if (!IsThreadAFiber() && !ConvertThreadToFiber(nullptr)) return;
    // Without a fiber, the sequence runs synchronously as before.
    fiber_ = CreateFiber(0, entry, this);
}
BackgroundShutdown::~BackgroundShutdown() {
    if (started_ && !done_) finish(nullptr);
    if (fiber_) DeleteFiber(fiber_);
}
void BackgroundShutdown::run() {
    // Components report their own shutdown errors. A failure must still
    // release the project, as destroying it synchronously did.
    try { sequence_(*this); } catch (...) { }
    done_ = true;
}
void WINAPI BackgroundShutdown::entry(void* parameter) {
    auto& self = *static_cast<BackgroundShutdown*>(parameter);
    self.run();
    // A fiber procedure must never return; the owner deletes this fiber.
    for (;;) SwitchToFiber(self.caller_);
}
void BackgroundShutdown::enter() {
    // MinGW's inline GetCurrentFiber reads the TEB through gs:[0x20], which
    // GCC misreports as an out-of-bounds access.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    caller_ = GetCurrentFiber();
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    SwitchToFiber(fiber_);
}
void BackgroundShutdown::start() {
    if (started_) return;
    started_ = true;
    if (!fiber_) { synchronous_ = true; run(); return; }
    enter();
}
void BackgroundShutdown::resume() {
    if (started_ && !done_ && task_ && taskDone_) enter();
}
void BackgroundShutdown::finish(ShutdownProgress* progress) {
    if (done_) return;
    progress_ = progress; synchronous_ = true;
    if (!started_) { start(); return; }
    if (task_ && !taskDone_) {
        const HANDLE task = task_;
        const auto join = [task] { WaitForSingleObject(task, INFINITE); };
        if (progress) progress->wait(join); else join();
    }
    enter();
}
void BackgroundShutdown::wait(const std::function<void()>& task) {
    if (synchronous_) { if (progress_) progress_->wait(task); else task(); return; }
    std::exception_ptr failure;
    taskDone_ = false;
    NativeThread worker([&] {
        try { task(); } catch (...) { failure = std::current_exception(); }
        // The handle signals only after the thread exits, so report completion first.
        taskDone_ = true;
        PostMessageW(notify_, message_, id_, 0);
    });
    task_ = worker.native_handle();
    // The stack stays alive while suspended, so the task may use its references.
    while (!taskDone_) SwitchToFiber(caller_);
    task_ = nullptr;
    worker.join();
    if (failure) std::rethrow_exception(failure);
}
}
