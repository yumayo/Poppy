#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace poppy {
// current.txt contains a content hash, never a path supplied to LoadLibrary.
inline std::string parseReleaseId(std::string_view value) {
    if (!value.empty() && value.back() == '\n') {
        value.remove_suffix(1);
        if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
    }
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string_view::npos)
        throw std::invalid_argument("Invalid current.txt. Publish a complete Poppy release before starting the application.");
    return std::string(value);
}
}
