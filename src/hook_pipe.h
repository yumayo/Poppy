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
    void stop();
    bool running() const { return running_; }
private:
    Handle process_, input_, output_, job_;
    NativeThread reader_, uploader_;
    std::atomic<bool> stopping_{false}, running_{false};
};
}
