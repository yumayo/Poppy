#pragma once
#include "win.h"
#include <optional>

namespace poppy {
std::optional<ResourceId> chooseWslFolder(HWND owner, const std::vector<std::string>& distributions);
}
