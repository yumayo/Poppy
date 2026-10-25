#pragma once
#include <string>
#include <string_view>

namespace poppy {
// Names accepted by the Win32 file APIs; no path separators or implicit trimming.
bool validFileName(std::wstring_view name, std::wstring& error);
}
