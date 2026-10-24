#include "session.h"
#include <cstdint>

namespace poppy {
std::string projectPathHash(std::string_view key) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char c : key) { hash ^= c; hash *= UINT64_C(1099511628211); }
    std::string result(16, '0');
    for (size_t i = 0; i < result.size(); ++i) {
        result[result.size() - i - 1] = "0123456789abcdef"[hash & 15]; hash >>= 4;
    }
    return result;
}
std::string projectDirectoryName(std::string_view key) {
    std::string result = "-";
    for (unsigned char c : key) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') result += static_cast<char>(c);
        else if (result.back() != '-') result += '-';
    }
    while (result.size() > 1 && result.back() == '-') result.pop_back();
    if (result.size() > 120) { result.resize(120); result += '-' + projectPathHash(key); }
    return result;
}
}
