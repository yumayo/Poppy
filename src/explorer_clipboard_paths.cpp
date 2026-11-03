#include "explorer_clipboard_paths.h"
#include <algorithm>

namespace poppy {
ResourceId explorerClipboardPath(std::wstring_view path, const ResourceId& destination) {
    if (const auto location = wslUncLocation(resourceUtf8(path))) {
        const auto sameDistro = [](std::string a, std::string b) {
            const auto lower = [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); };
            std::transform(a.begin(), a.end(), a.begin(), lower);
            std::transform(b.begin(), b.end(), b.begin(), lower);
            return a == b;
        };
        if (destination.wsl() && sameDistro(location->distro, destination.location().distro))
            return destination.at(location->root);
        return ResourceId::wsl(location->distro, location->root);
    }
    return std::filesystem::path(path);
}
std::optional<std::wstring> explorerShellPath(const ResourceId& path) {
    auto name = path.native();
    if (path.wsl()) {
        if (name.find(L'\\') != name.npos) return {};
        std::replace(name.begin(), name.end(), L'/', L'\\');
        name = L"\\\\wsl.localhost\\" + resourceWide(path.location().distro) + name;
    }
    return name;
}
}
