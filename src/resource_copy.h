#pragma once
#include <filesystem>

namespace poppy {
// Copy through private staging, then publish without replacing a destination.
// Linux links are copied as links; Windows reparse points are rejected.
// Imports from Windows also reject links seen through WSL's drive mounts.
void copyResourcePath(const std::filesystem::path& source, const std::filesystem::path& destination, bool rejectLinks = false);
}
