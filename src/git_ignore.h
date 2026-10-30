#pragma once
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace poppy {
// Run in the listed directory. Exit codes 0 (matches) and 1 (no matches)
// return stdout; other failures return nullopt and leave names undimmed.
using GitIgnoreRun = std::function<std::optional<std::string>(const std::vector<std::string>&)>;
std::unordered_set<std::string> gitIgnoredNames(const std::vector<std::string>& names, const GitIgnoreRun& run);
}
