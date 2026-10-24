#pragma once
#include "win.h"
#include "ai_status.h"
#include <set>

namespace poppy {
class Renderer;
constexpr UINT wmAiEvent = WM_APP + 30;
std::string newSessionId();
// One process-wide listener. Registrations live exactly as long as terminal starts.
class HookServer {
public:
    ~HookServer();
    bool start(HWND target);
    void stop();
    void add(const std::string& id);
    void remove(const std::string& id);
    std::vector<AiEvent> drain();
    const std::string& url() const { return url_; }
    const std::wstring& error() const { return error_; }
private:
    void serve();
    HWND target_ = nullptr;
    uintptr_t listener_ = static_cast<uintptr_t>(-1);
    bool winsock_ = false;
    std::atomic<bool> stopping_{false};
    NativeThread thread_;
    std::mutex mutex_;
    std::set<std::string> ids_;
    std::deque<AiEvent> events_;
    std::string url_;
    std::wstring error_;
};
void drawAiStatus(Renderer& renderer, RECT rect, AiState state, UINT dpi, unsigned frame = 0);
}
