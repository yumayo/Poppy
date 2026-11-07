#pragma once
#include <filesystem>

namespace poppy {
// Only offer updates for the versioned installation containing the running DLL.
bool applicationUpdateAvailable(const std::filesystem::path& installation, const std::filesystem::path& runningRelease);
}
