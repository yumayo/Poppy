#pragma once
#include <string>
#include <string_view>

namespace poppy {
// Detect only explicit HTTP(S) URLs. Unsafe characters and malformed escapes
// terminate the URL; an invalid authority or an oversized URL is rejected.
// Coordinates refer to displayed cells, including wide-character continuations.
std::string terminalLinkAt(std::u32string_view text, size_t position);
}
