#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace poppy::wsl {
constexpr unsigned protocolVersion = 1;
constexpr const char* helperVersion = "5";
constexpr size_t maxControl = 64 * 1024, maxChunk = 256 * 1024, maxFrame = 1024 * 1024;
// Flat JSON control messages. Lists are streamed as individual control frames;
// bodies use binary frames, never JSON strings or base64.
using Message = std::map<std::string, std::string>;
std::string encodeMessage(const Message& message);
Message decodeMessage(std::string_view text);
uint64_t number(const Message& message, const std::string& field);
std::string field(const Message& message, const std::string& name);
struct Frame { uint8_t kind = 1; std::string payload; };
std::string encodeFrame(const Frame& frame);
class FrameReader {
public:
    void feed(std::string_view bytes, const std::function<void(Frame)>& receive);
    void finish() const;
private:
    std::string pending_;
    uint32_t size_ = 0;
};
struct Chunk { uint64_t id, offset; bool last; std::string_view bytes; };
Frame dataFrame(uint64_t id, uint64_t offset, bool last, std::string_view bytes);
Chunk decodeChunk(const Frame& frame);
std::string sha256(std::string_view bytes);
}
