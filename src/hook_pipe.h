#pragma once
#include "win.h"
#include "wsl_protocol.h"

namespace poppy {
// A terminal owns one anonymous-pipe connection to its WSL notification socket.
// Routing to a Poppy session is decided here, outside the AI container.
class HookPipe {
public:
    ~HookPipe() { stop(); }
    std::string start(const std::vector<std::wstring>& command, std::function<void(const wsl::Message&)> receive, std::string input = {});
    bool stop();
    bool running() const { return running_; }
private:
    std::string startAttempt(const std::vector<std::wstring>& command, std::function<void(const wsl::Message&)> receive,
        const std::string& input, ULONGLONG deadline);
    Handle process_, input_, output_;
    ProcessJob job_;
    NativeThread reader_, uploader_;
    std::atomic<bool> stopping_{false}, running_{false};
    bool ready_ = false;
};
}
