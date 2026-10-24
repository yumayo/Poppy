#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
enum class AiState { Idle, Running, Waiting, WaitingForInput, WaitingForApproval, Paused, Completed, Failed, Unknown, Count };
struct AiEvent {
    std::string sessionId;
    std::optional<AiState> state;
    bool onlyIfActive = false;
    std::optional<std::string> activity;
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
// Strict, bounded WpfTaskBar JSON. No inference from activity text or silence.
std::optional<AiEvent> parseAiEvent(std::string_view id, bool activityOnly, std::string_view body);
std::string jsonString(std::string_view text);
}
