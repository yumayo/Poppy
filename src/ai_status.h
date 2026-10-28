#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace poppy {
enum class AiState { Idle, Running, Waiting, WaitingForInput, WaitingForApproval, Paused, Completed, Failed, Unknown, Count };
struct AiEvent {
    std::string sessionId;
    std::optional<AiState> state;
    bool onlyIfActive = false;
    std::optional<std::string> activity;
};
// Bounded in-process delivery from pipe readers to the UI. IDs live only as
// long as their owning terminal; removing one also drops its queued events.
class AiEventQueue {
public:
    explicit AiEventQueue(std::function<void()> notify = {}) : notify_(std::move(notify)) {}
    void add(const std::string& id);
    void remove(const std::string& id);
    bool publish(const std::string& id, bool activity, std::string_view body);
    std::vector<AiEvent> drain();
private:
    std::function<void()> notify_;
    std::mutex mutex_;
    std::set<std::string> ids_;
    std::deque<AiEvent> events_;
};
enum class AiSource { None, Hook, Process };
enum class AiConfidence { Unknown, Confirmed };
struct AiStatus {
    AiState state = AiState::Idle;
    AiSource source = AiSource::None;
    AiConfidence confidence = AiConfidence::Unknown;
    bool hookObserved = false;
    bool detected = false, unread = false;
    std::string activity;
    uint64_t changedAt = 0;
    bool apply(const AiEvent& event, uint64_t now, bool visible);
    void acknowledge() { unread = false; }
};
struct AiSummary {
    AiState state = AiState::Idle;
    bool unread = false;
    std::array<size_t, static_cast<size_t>(AiState::Count)> counts{};
};
AiSummary summarizeAi(const std::vector<AiStatus>& terminals);
const char* aiStateName(AiState state);
bool validNotificationId(std::string_view id);
// Strict, bounded Poppy status JSON. No inference from activity text or silence.
std::optional<AiEvent> parseAiEvent(std::string_view id, bool activityOnly, std::string_view body);
std::string jsonString(std::string_view text);
}
