#include "terminal_link.h"
#include <algorithm>

namespace poppy {
namespace {
bool alpha(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }
bool digit(char32_t c) { return c >= U'0' && c <= U'9'; }
int hex(char32_t c) {
    if (digit(c)) return c - U'0';
    if (c >= U'a' && c <= U'f') return c - U'a' + 10;
    if (c >= U'A' && c <= U'F') return c - U'A' + 10;
    return -1;
}
bool ipv4(std::u32string_view host) {
    int parts = 0;
    for (size_t start = 0; start < host.size();) {
        const auto end = host.find(U'.', start);
        const auto part = host.substr(start, end == host.npos ? host.size() - start : end - start);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == U'0')) return false;
        int value = 0;
        for (auto c : part) { if (!digit(c)) return false; value = value * 10 + c - U'0'; }
        if (value > 255 || ++parts > 4) return false;
        if (end == host.npos) return parts == 4;
        start = end + 1;
    }
    return false;
}
bool ipv6(std::u32string_view host) {
    const auto compression = host.find(U"::");
    if (compression != host.npos && host.find(U"::", compression + 2) != host.npos) return false;
    int groups = 0;
    for (size_t start = 0; start < host.size();) {
        if (host[start] == U':') {
            if (compression == host.npos || (start != compression && start != compression + 1)) return false;
            ++start; continue;
        }
        const auto end = host.find(U':', start);
        const auto part = host.substr(start, end == host.npos ? host.size() - start : end - start);
        if (part.find(U'.') != part.npos) {
            if (end != host.npos || !ipv4(part)) return false;
            groups += 2;
        } else {
            if (part.size() > 4 || !std::all_of(part.begin(), part.end(), [](char32_t c) { return hex(c) >= 0; })) return false;
            ++groups;
        }
        if (end == host.npos) break;
        if (end + 1 == host.size() && (compression == host.npos || end != compression + 1)) return false;
        start = end + 1;
    }
    return compression == host.npos ? groups == 8 : groups < 8;
}
bool authority(std::u32string_view value) {
    if (value.empty()) return false;
    std::u32string_view host, port;
    bool hasPort = false;
    if (value.front() == U'[') {
        const auto end = value.find(U']');
        if (end == value.npos || !ipv6(value.substr(1, end - 1))) return false;
        if (end + 1 < value.size()) {
            if (value[end + 1] != U':') return false;
            hasPort = true; port = value.substr(end + 2);
        }
    } else {
        const auto colon = value.find(U':');
        host = value.substr(0, colon);
        if (colon != value.npos) { hasPort = true; port = value.substr(colon + 1); }
        if (host.empty() || host.size() > 253) return false;
        const bool numeric = std::all_of(host.begin(), host.end(), [](char32_t c) { return digit(c) || c == U'.'; });
        if (numeric) { if (!ipv4(host)) return false; }
        else for (size_t start = 0; start < host.size();) {
            const auto end = host.find(U'.', start);
            const auto label = host.substr(start, end == host.npos ? host.size() - start : end - start);
            if (label.empty() || label.size() > 63 || label.front() == U'-' || label.back() == U'-' ||
                !std::all_of(label.begin(), label.end(), [](char32_t c) { return alpha(c) || digit(c) || c == U'-'; })) return false;
            if (end == host.npos) break;
            start = end + 1;
        }
    }
    if (hasPort) {
        if (port.empty() || port.size() > 5) return false;
        unsigned number = 0;
        for (auto c : port) { if (!digit(c)) return false; number = number * 10 + c - U'0'; }
        if (!number || number > 65535) return false;
    }
    return true;
}
bool urlCharacter(char32_t c) {
    return alpha(c) || digit(c) || std::u32string_view(U"-._~:/?#[]@!$&()*+,;=%").find(c) != std::u32string_view::npos;
}
bool embedded(char32_t c) {
    return alpha(c) || digit(c) || std::u32string_view(U"_+.-/:@%\\").find(c) != std::u32string_view::npos;
}
bool scheme(std::u32string_view text, size_t start, std::u32string_view expected) {
    if (expected.size() > text.size() - start) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        auto c = text[start + i]; if (c >= U'A' && c <= U'Z') c += U'a' - U'A';
        if (c != expected[i]) return false;
    }
    return true;
}
}
std::string terminalLinkAt(std::u32string_view text, size_t position) {
    if (position >= text.size()) return {};
    constexpr size_t maximum = 4096;
    for (size_t start = 0; start <= position; ++start) {
        if (start && embedded(text[start - 1])) continue;
        const size_t prefix = scheme(text, start, U"https://") ? 8 : scheme(text, start, U"http://") ? 7 : 0;
        if (!prefix) continue;
        size_t end = start + prefix;
        int parentheses = 0, brackets = 0;
        for (; end < text.size() && urlCharacter(text[end]) && end - start <= maximum; ++end) {
            if (text[end] == U'%') {
                if (end + 2 >= text.size() || hex(text[end + 1]) < 0 || hex(text[end + 2]) < 0) break;
                const auto decoded = hex(text[end + 1]) * 16 + hex(text[end + 2]);
                if (decoded < 32 || decoded == 127 || decoded == '\\') break;
                end += 2;
            } else if (text[end] == U'(') ++parentheses;
            else if (text[end] == U')') { if (!parentheses) break; --parentheses; }
            else if (text[end] == U'[') ++brackets;
            else if (text[end] == U']') { if (!brackets) break; --brackets; }
        }
        // Do not turn an overlong URL into a different, truncated destination.
        if (end - start > maximum) {
            while (end < text.size() && urlCharacter(text[end])) ++end;
            if (position < end) return {};
            start = end - 1; continue;
        }
        while (end > start + prefix && std::u32string_view(U".,;!?").find(text[end - 1]) != text.npos) --end;
        const auto candidate = text.substr(start, end - start);
        const auto hostEnd = candidate.find_first_of(U"/?#", prefix);
        if (position >= start && position < end && authority(candidate.substr(prefix, hostEnd == candidate.npos ? candidate.size() - prefix : hostEnd - prefix))) {
            std::string result; result.reserve(candidate.size());
            for (auto c : candidate) result += static_cast<char>(c);
            return result;
        }
        if (end > position) return {};
        start = std::max(start, end ? end - 1 : end);
    }
    return {};
}
}
