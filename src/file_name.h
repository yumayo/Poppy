#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
// Names accepted by the Win32 file APIs; no path separators or implicit trimming.
bool validFileName(std::wstring_view name, std::wstring& error);
constexpr size_t maxCreationPathLength = 4096;
struct CreationPath {
    std::vector<std::wstring> components;
    bool directory = false;
};
// Relative paths use / on both platforms; a final / requests a directory.
std::optional<CreationPath> parseCreationPath(std::wstring_view name, bool directory, bool posix, std::wstring& error);
}
