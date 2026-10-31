#pragma once
#include <filesystem>

namespace poppy {
// Copy through private staging, then publish without replacing a destination.
// Linux links are copied as links; Windows reparse points are rejected.
void copyResourcePath(const std::filesystem::path& source, const std::filesystem::path& destination);
}
