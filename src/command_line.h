#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
// Encode a CreateProcess command line. WSL inspects its option prefix before
// decoding quoted arguments, so options such as --list must stay unquoted.
std::wstring quoteArg(std::wstring_view arg);
std::wstring processCommandLine(const std::vector<std::wstring>& args);
}
