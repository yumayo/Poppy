#pragma once
#include <string>
#include <vector>

namespace poppy {
struct StartupOptions {
    std::vector<std::wstring> projects;
};
// Arguments exclude the executable name. Throws on unknown or incomplete options.
StartupOptions parseStartupOptions(const std::vector<std::wstring>& arguments);
}
