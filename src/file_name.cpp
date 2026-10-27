#include "file_name.h"
#include "resource.h"
#include <algorithm>

namespace poppy {
bool validFileName(std::wstring_view name, std::wstring& error) {
    if (name.empty() || name == L"." || name == L".." || name.size() > 255) { error = L"Enter a name between 1 and 255 characters."; return false; }
    if (name.back() == L'.' || name.back() == L' ') { error = L"A name cannot end with a dot or a space."; return false; }
    for (const auto c : name) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) { error = L"A name cannot contain control characters or < > : \" / \\ | ? *."; return false; }
    auto base = std::wstring(name.substr(0, name.find(L'.')));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t c) { return c >= L'a' && c <= L'z' ? c - L'a' + L'A' : c; });
    const bool device = base.size() == 4 && (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0) &&
        ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'¹' || base[3] == L'²' || base[3] == L'³');
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CONIN$" || base == L"CONOUT$" || device) { error = L"This name is reserved by Windows."; return false; }
    error.clear(); return true;
}
std::optional<CreationPath> parseCreationPath(std::wstring_view name, bool directory, bool posix, std::wstring& error) {
    if (name.empty() || name.size() > maxCreationPathLength) {
        error = L"Enter a relative path between 1 and 4096 characters."; return std::nullopt;
    }
    CreationPath result;
    result.directory = directory || name.back() == L'/';
    if (name.back() == L'/') name.remove_suffix(1);
    size_t start = 0;
    for (;;) {
        const auto end = name.find(L'/', start);
        const auto component = name.substr(start, end == name.npos ? name.size() - start : end - start);
        if (posix) {
            if (!validPosixName(resourceUtf8(component))) {
                error = L"Each path component must be a valid Linux filename (no empty name, NUL, . or ..; at most 255 UTF-8 bytes).";
                return std::nullopt;
            }
        } else if (!validFileName(component, error)) return std::nullopt;
        result.components.emplace_back(component);
        if (end == name.npos) break;
        start = end + 1;
    }
    error.clear(); return result;
}
}
