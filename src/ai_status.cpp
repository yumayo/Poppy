#include "ai_status.h"
#include "text.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace poppy {
namespace {
bool active(AiState state) { return state == AiState::Running || state == AiState::Waiting || state == AiState::WaitingForInput || state == AiState::WaitingForApproval; }
int priority(AiState state) {
    switch (state) {
    case AiState::Waiting: case AiState::WaitingForInput: case AiState::WaitingForApproval: return 7;
    case AiState::Failed: return 6; case AiState::Running: return 5; case AiState::Paused: return 4;
    case AiState::Completed: return 3; case AiState::Unknown: return 2; default: return 0;
    }
}
// The protocol accepts a flat object with string/bool/null fields only.
// Reject duplicate fields, invalid UTF-8, malformed escapes and trailing data.
struct Value { std::string text; char type = 0; };
class Json {
public:
    explicit Json(std::string_view text) : text_(text) {}
    std::map<std::string, Value> object() {
        std::map<std::string, Value> fields;
        expect('{'); space();
        if (!take('}')) for (;;) {
            auto key = string(); expect(':'); space(); Value value;
            if (peek() == '"') { value.type = 's'; value.text = string(); }
            else if (literal("true")) value.type = 't';
            else if (literal("false")) value.type = 'f';
            else if (literal("null")) value.type = 'n';
            else throw std::runtime_error("Invalid field");
            if (!fields.emplace(std::move(key), std::move(value)).second || fields.size() > 16) throw std::runtime_error("Duplicate/extra fields");
            space(); if (take('}')) break; expect(',');
        }
        space(); if (pos_ != text_.size()) throw std::runtime_error("Trailing JSON");
        return fields;
    }
private:
    char peek() const { return pos_ < text_.size() ? text_[pos_] : 0; }
    void space() { while (peek() == ' ' || peek() == '\r' || peek() == '\n' || peek() == '\t') ++pos_; }
    bool take(char ch) { space(); if (peek() != ch) return false; ++pos_; return true; }
    void expect(char ch) { if (!take(ch)) throw std::runtime_error("Invalid JSON"); }
    bool literal(std::string_view word) { if (text_.substr(pos_, word.size()) != word) return false; pos_ += word.size(); return true; }
    unsigned hex4() {
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            const auto ch = peek(); if (!ch) throw std::runtime_error("Short escape"); ++pos_;
            const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (digit < 0) throw std::runtime_error("Invalid escape");
            result = result * 16 + static_cast<unsigned>(digit);
        }
        return result;
    }
    std::string string() {
        expect('"'); std::string result;
        while (peek() != '"') {
            const auto ch = static_cast<unsigned char>(peek());
            if (ch < 32) throw std::runtime_error("Invalid string");
            ++pos_;
            if (ch != '\\') { result += static_cast<char>(ch); continue; }
            const auto escaped = peek(); if (!escaped) throw std::runtime_error("Short escape"); ++pos_;
            switch (escaped) {
            case '"': case '\\': case '/': result += escaped; break;
            case 'b': result += '\b'; break; case 'f': result += '\f'; break; case 'n': result += '\n'; break; case 'r': result += '\r'; break; case 't': result += '\t'; break;
            case 'u': {
                auto code = hex4();
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (peek() != '\\') throw std::runtime_error("Missing low surrogate");
                    ++pos_; if (peek() != 'u') throw std::runtime_error("Missing low surrogate"); ++pos_;
                    const auto low = hex4(); if (low < 0xdc00 || low > 0xdfff) throw std::runtime_error("Invalid low surrogate");
                    code = 0x10000 + (code - 0xd800) * 1024 + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) throw std::runtime_error("Unexpected surrogate");
                appendUtf8(result, code); break;
            }
            default: throw std::runtime_error("Invalid escape");
            }
        }
        ++pos_;
        size_t count = 0;
        for (size_t i = 0; i < result.size();) {
            const auto ch = static_cast<unsigned char>(result[i]);
            size_t length = ch < 0x80 ? 1 : ch >= 0xc2 && ch <= 0xdf ? 2 : ch >= 0xe0 && ch <= 0xef ? 3 : ch >= 0xf0 && ch <= 0xf4 ? 4 : 0;
            if (!length || i + length > result.size()) throw std::runtime_error("Invalid UTF-8");
            for (size_t n = 1; n < length; ++n) if ((static_cast<unsigned char>(result[i+n]) & 0xc0) != 0x80) throw std::runtime_error("Invalid UTF-8");
            if (length >= 3) {
                const auto next = static_cast<unsigned char>(result[i+1]);
                if ((ch == 0xe0 && next < 0xa0) || (ch == 0xed && next >= 0xa0) || (ch == 0xf0 && next < 0x90) || (ch == 0xf4 && next >= 0x90)) throw std::runtime_error("Invalid UTF-8");
            }
            i += length; if (++count > 512) throw std::runtime_error("String too long");
        }
        return result;
    }
    std::string_view text_; size_t pos_ = 0;
};
}
bool AiStatus::apply(const AiEvent& event, uint64_t now, bool visible) {
    if (event.onlyIfActive && !active(state)) return false;
    if (!event.state && !active(state)) return false;
    hookObserved = true; source = AiSource::Hook; confidence = AiConfidence::Confirmed;
    if (event.state) {
        const auto next = *event.state;
        if (state != next) {
            changedAt = now;
            if (!visible && (next == AiState::Waiting || next == AiState::WaitingForInput || next == AiState::WaitingForApproval || next == AiState::Completed || next == AiState::Failed || next == AiState::Paused)) unread = true;
        }
        state = next; detected = next != AiState::Idle;
        if (next == AiState::Idle) { activity.clear(); unread = false; }
        else if (event.activity) activity = *event.activity;
        else activity.clear();
    } else if (event.activity) activity = *event.activity;
    if (visible) acknowledge();
    return true;
}
AiSummary summarizeAi(const std::vector<AiStatus>& terminals) {
    AiSummary result;
    for (const auto& terminal : terminals) if (terminal.detected) {
        ++result.counts[static_cast<size_t>(terminal.state)]; result.unread |= terminal.unread;
        if (priority(terminal.state) > priority(result.state)) result.state = terminal.state;
    }
    return result;
}
const char* aiStateName(AiState state) {
    switch (state) {
    case AiState::Running: return "Running"; case AiState::Waiting: return "Waiting for response";
    case AiState::WaitingForInput: return "Waiting for input"; case AiState::WaitingForApproval: return "Waiting for approval";
    case AiState::Paused: return "Interrupted"; case AiState::Completed: return "Completed";
    case AiState::Failed: return "Failed"; case AiState::Unknown: return "Unknown"; default: return "Idle";
    }
}
bool validNotificationId(std::string_view id) { return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string_view::npos; }
std::optional<AiEvent> parseAiEvent(std::string_view id, bool activityOnly, std::string_view body) {
    if (!validNotificationId(id) || body.size() > 8192) return {};
    try {
        auto fields = Json(body).object(); AiEvent event; event.sessionId = id;
        const auto status = fields.find("status"), activity = fields.find("activityText"), condition = fields.find("onlyIfActive");
        if (activity != fields.end()) { if (activity->second.type == 's') event.activity = activity->second.text; else if (activity->second.type != 'n') return {}; }
        if (condition != fields.end()) { if (condition->second.type != 't' && condition->second.type != 'f') return {}; event.onlyIfActive = condition->second.type == 't'; }
        if (activityOnly) { if (!event.activity || status != fields.end()) return {}; }
        else {
            if (status == fields.end() || status->second.type != 's') return {};
            const auto& value = status->second.text;
            if (value == "running") event.state = AiState::Running; else if (value == "waiting") event.state = AiState::Waiting;
            else if (value == "interrupted") event.state = AiState::Paused; else if (value == "completed") event.state = AiState::Completed;
            else if (value == "none") event.state = AiState::Idle; else return {};
        }
        return event;
    } catch (const std::exception&) { return {}; }
}
void AiEventQueue::add(const std::string& id) { std::lock_guard<std::mutex> lock(mutex_); ids_.insert(id); }
void AiEventQueue::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_); ids_.erase(id);
    events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const AiEvent& event) { return event.sessionId == id; }), events_.end());
}
bool AiEventQueue::publish(const std::string& id, bool activity, std::string_view body) {
    const auto event = parseAiEvent(id, activity, body); if (!event) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ids_.count(id) || events_.size() >= 1024) return false;
        events_.push_back(*event);
    }
    if (notify_) notify_();
    return true;
}
std::vector<AiEvent> AiEventQueue::drain() {
    std::lock_guard<std::mutex> lock(mutex_); std::vector<AiEvent> result;
    while (!events_.empty()) { result.push_back(std::move(events_.front())); events_.pop_front(); }
    return result;
}
std::string jsonString(std::string_view text) {
    std::string result = "\""; constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += static_cast<char>(ch);
    }
    return result + '"';
}
}
