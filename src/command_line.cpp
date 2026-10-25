#include "command_line.h"

namespace poppy {
std::wstring quoteArg(std::wstring_view arg) {
    // Keep WSL switches visible to its initial command-line parser. Arguments
    // containing whitespace/quotes still need the standard Windows escaping.
    if (!arg.empty() && arg.find_first_of(L" \t\r\n\v\f\"") == arg.npos) return std::wstring(arg);
    std::wstring result = L"\""; size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') ++slashes;
        else { result.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0; if (c == L'"') result += L'\\'; result += c; }
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
std::wstring processCommandLine(const std::vector<std::wstring>& args) {
    std::wstring command;
    for (const auto& arg : args) { if (!command.empty()) command += L' '; command += quoteArg(arg); }
    return command;
}
}
