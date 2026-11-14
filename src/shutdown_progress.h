#pragma once
#include <functional>
#include <string>

namespace poppy {
// Receives the blocking steps of a shutdown. Code between waits runs on the
// caller's UI thread; only tasks passed to wait() may run on another thread.
class ShutdownProgress {
public:
    virtual ~ShutdownProgress() = default;
    virtual void detail(std::wstring text) = 0;
    // The task must contain only blocking I/O or waits, never UI operations.
    // Returns after the task has finished and rethrows its exception.
    virtual void wait(const std::function<void()>& task) = 0;
};
}
