#include "file_name.h"
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
}
