#include "html_preview.h"
#include <algorithm>
#include <stdexcept>

namespace poppy {
bool isHtmlExtension(std::wstring_view extension) {
    std::wstring lower(extension);
    for (auto& c : lower) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return lower == L".html" || lower == L".htm";
}
std::string htmlFileUri(const ResourceId& resource) {
    std::string path;
    if (resource.wsl()) {
        if (resource.u8string().find('\\') != std::string::npos)
            throw std::runtime_error("This WSL filename cannot be opened through the Windows WSL share.");
        if (!resource.is_absolute()) throw std::runtime_error("An absolute WSL path is required for HTML preview.");
        path = "//wsl.localhost/" + resource.location().distro + resource.u8string();
    } else {
        path = resource.generic_u8string();
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.compare(0, 8, "//?/UNC/") == 0) path = "//" + path.substr(8);
        else if (path.compare(0, 4, "//?/") == 0) path.erase(0, 4);
    }
    if (path.empty()) throw std::runtime_error("No HTML file selected.");
    std::string uri = path.compare(0, 2, "//") == 0 ? "file:" : path.front() == '/' ? "file://" : "file:///";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == ':' || c == '-' || c == '.' || c == '_' || c == '~') uri += static_cast<char>(c);
        else { uri += '%'; uri += hex[c >> 4]; uri += hex[c & 15]; }
    }
    return uri;
}
}
