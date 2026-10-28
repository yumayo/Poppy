#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
struct StartupOptions {
    std::vector<std::wstring> projects;
};
// Arguments exclude the executable name. Throws on unknown or incomplete options.
StartupOptions parseStartupOptions(const std::vector<std::wstring>& arguments);
// Resolve local paths against the caller's directory, preserving WSL UNC paths.
std::wstring resolveStartupProjectPath(std::wstring_view project);
bool isAbsoluteStartupProjectPath(std::wstring_view project);
}
