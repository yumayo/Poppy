#include "wsl_protocol.h"
#include "resource.h"
#include "text.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace poppy::wsl {
namespace {
[[noreturn]] void invalid() { throw std::runtime_error("Invalid WSL protocol data"); }
void put(std::string& out, uint64_t value, unsigned count) { for (unsigned i = count; i; --i) out += static_cast<char>(value >> ((i - 1) * 8)); }
uint64_t get(std::string_view bytes) { uint64_t value = 0; for (unsigned char c : bytes) value = (value << 8) | c; return value; }
std::string quoteJson(std::string_view text) {
    if (!validUtf8(text)) invalid();
    std::string out = "\"";
    for (const unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
        else if (ch < 32) { out += "\\u00"; out += "0123456789abcdef"[ch >> 4]; out += "0123456789abcdef"[ch & 15]; }
        else out += static_cast<char>(ch);
    }
    return out + '"';
}
}
std::string encodeMessage(const Message& message) {
    std::string out = "{";
    for (const auto& item : message) { if (out.size() > 1) out += ','; out += quoteJson(item.first) + ':' + quoteJson(item.second); }
    out += '}'; if (out.size() > maxControl) throw std::runtime_error("WSL control message exceeds 64 KiB"); return out;
}
Message decodeMessage(std::string_view text) {
    if (text.size() > maxControl || !validUtf8(text)) invalid();
    size_t at = 0; auto space = [&] { while (at < text.size() && (text[at] == ' ' || text[at] == '\r' || text[at] == '\n' || text[at] == '\t')) ++at; };
    auto take = [&](char c) { space(); if (at == text.size() || text[at++] != c) invalid(); };
    auto hex = [&]() { unsigned value = 0; for (int i = 0; i < 4; ++i) { if (at == text.size()) invalid(); const char c = text[at++]; value <<= 4; if (c >= '0' && c <= '9') value |= c - '0'; else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10; else invalid(); } return value; };
    auto string = [&]() {
        take('"'); std::string out;
        for (;;) {
            if (at == text.size()) invalid();
            const unsigned char c = text[at++];
            if (c == '"') return out;
            if (c < 32) invalid();
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (at == text.size()) invalid();
            switch (text[at++]) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'u': { char32_t ch = hex(); if (ch >= 0xd800 && ch <= 0xdbff) { if (text.substr(at, 2) != "\\u") invalid(); at += 2; const auto low = hex(); if (low < 0xdc00 || low > 0xdfff) invalid(); ch = 0x10000 + ((ch - 0xd800) << 10) + low - 0xdc00; } else if (ch >= 0xdc00 && ch <= 0xdfff) invalid(); appendUtf8(out, ch); break; }
            default: invalid();
            }
        }
    };
    Message out; take('{'); space();
    if (at < text.size() && text[at] != '}') for (;;) {
        const auto key = string(); take(':'); space(); std::string value;
        if (at < text.size() && text[at] == '"') value = string();
        else { const auto begin = at; while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at; if (begin == at || (at - begin > 1 && text[begin] == '0')) invalid(); value = text.substr(begin, at - begin); }
        if (!out.emplace(key, std::move(value)).second) invalid();
        space();
        if (at == text.size() || text[at] != ',') break;
        ++at;
    }
    take('}'); space(); if (at != text.size()) invalid(); return out;
}
std::string field(const Message& message, const std::string& key) { const auto it = message.find(key); return it == message.end() ? "" : it->second; }
uint64_t number(const Message& message, const std::string& key) {
    const auto value = field(message, key); if (value.empty()) invalid(); uint64_t out = 0;
    for (const char c : value) { if (c < '0' || c > '9' || out > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10) invalid(); out = out * 10 + c - '0'; } return out;
}
std::string encodeFrame(const Frame& frame) {
    if ((frame.kind != 1 && frame.kind != 2) || frame.payload.size() > (frame.kind == 1 ? maxControl : maxChunk + 17)) invalid();
    std::string out; put(out, frame.payload.size() + 1, 4); out += static_cast<char>(frame.kind); out += frame.payload; return out;
}
void FrameReader::feed(std::string_view bytes, const std::function<void(Frame)>& receive) {
    while (!bytes.empty()) {
        const size_t needed = size_ ? size_ : 4;
        const size_t count = std::min(bytes.size(), needed - pending_.size()); pending_.append(bytes.substr(0, count)); bytes.remove_prefix(count);
        if (pending_.size() != needed) continue;
        if (!size_) { size_ = static_cast<uint32_t>(get(pending_)); pending_.clear(); if (size_ < 1 || size_ > maxFrame) invalid(); }
        else {
            Frame frame{static_cast<uint8_t>(pending_[0]), pending_.substr(1)}; pending_.clear(); size_ = 0;
            if ((frame.kind != 1 && frame.kind != 2) || frame.payload.size() > (frame.kind == 1 ? maxControl : maxChunk + 17)) invalid();
            receive(std::move(frame));
        }
    }
}
void FrameReader::finish() const { if (size_ || !pending_.empty()) throw std::runtime_error("Truncated WSL frame"); }
Frame dataFrame(uint64_t id, uint64_t offset, bool last, std::string_view bytes) { if (bytes.size() > maxChunk) invalid(); Frame out{2, {}}; put(out.payload, id, 8); put(out.payload, offset, 8); out.payload += last ? '\1' : '\0'; out.payload += bytes; return out; }
Chunk decodeChunk(const Frame& frame) { if (frame.kind != 2 || frame.payload.size() < 17 || frame.payload.size() > maxChunk + 17 || static_cast<unsigned char>(frame.payload[16]) > 1) invalid(); return {get(std::string_view(frame.payload).substr(0, 8)), get(std::string_view(frame.payload).substr(8, 8)), frame.payload[16] != 0, std::string_view(frame.payload).substr(17)}; }

std::string sha256(std::string_view bytes) {
    static constexpr uint32_t k[] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<uint32_t,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    auto rotate = [](uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };
    const size_t blocks = (bytes.size() + 9 + 63) / 64;
    for (size_t block = 0; block < blocks; ++block) {
        uint32_t w[64]{};
        for (size_t i = 0; i < 64; ++i) { const auto at = block * 64 + i; uint8_t c = 0; if (at < bytes.size()) c = static_cast<uint8_t>(bytes[at]); else if (at == bytes.size()) c = 128; else if (at >= blocks * 64 - 8) c = static_cast<uint8_t>((static_cast<uint64_t>(bytes.size()) * 8) >> ((blocks * 64 - 1 - at) * 8)); w[i / 4] |= static_cast<uint32_t>(c) << (24 - (i % 4) * 8); }
        for (size_t i = 16; i < 64; ++i) { const auto a = w[i-15], b = w[i-2]; w[i] = w[i-16] + (rotate(a,7)^rotate(a,18)^(a>>3)) + w[i-7] + (rotate(b,17)^rotate(b,19)^(b>>10)); }
        auto v = h;
        for (size_t i = 0; i < 64; ++i) { const auto t = v[7]+(rotate(v[4],6)^rotate(v[4],11)^rotate(v[4],25))+((v[4]&v[5])^(~v[4]&v[6]))+k[i]+w[i]; const auto s = (rotate(v[0],2)^rotate(v[0],13)^rotate(v[0],22))+((v[0]&v[1])^(v[0]&v[2])^(v[1]&v[2])); v = {t+s,v[0],v[1],v[2],v[3]+t,v[4],v[5],v[6]}; }
        for (size_t i = 0; i < 8; ++i) h[i] += v[i];
    }
    std::string out; for (const auto v : h) for (int i = 7; i >= 0; --i) out += "0123456789abcdef"[(v >> (i * 4)) & 15]; return out;
}
}
