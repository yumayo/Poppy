#include "resource.h"
#include "text.h"
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace poppy {
namespace {
std::string tuple(std::string_view value) { return std::to_string(value.size()) + ':' + std::string(value); }
std::string hex(std::string_view value) {
    std::string out;
    for (const unsigned char c : value) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
}
std::string unhex(std::string_view value) {
    if (value.size() % 2) throw std::runtime_error("Invalid WSL location");
    auto digit = [](char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; throw std::runtime_error("Invalid WSL location"); };
    std::string out;
    for (size_t i = 0; i < value.size(); i += 2) out += static_cast<char>(digit(value[i]) * 16 + digit(value[i + 1]));
    return out;
}
}
bool validUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const size_t start = i; const auto ch = decodeUtf8(text, i);
        if (ch == 0xfffd && text.substr(start, i - start) != "\xef\xbf\xbd") return false;
    }
    return true;
}
std::wstring resourceWide(std::string_view text) {
    std::wstring out;
    for (size_t i = 0; i < text.size();) {
        auto ch = decodeUtf8(text, i);
        if (sizeof(wchar_t) == 2 && ch > 0xffff) { ch -= 0x10000; out += static_cast<wchar_t>(0xd800 + (ch >> 10)); out += static_cast<wchar_t>(0xdc00 + (ch & 1023)); }
        else out += static_cast<wchar_t>(ch);
    }
    return out;
}
std::string resourceUtf8(std::wstring_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        char32_t ch = text[i];
        if (sizeof(wchar_t) == 2 && ch >= 0xd800 && ch <= 0xdbff) {
            if (i + 1 >= text.size() || text[i + 1] < 0xdc00 || text[i + 1] > 0xdfff) throw std::runtime_error("Invalid Unicode path");
            ch = 0x10000 + ((ch - 0xd800) << 10) + (text[++i] - 0xdc00);
        } else if (ch >= 0xdc00 && ch <= 0xdfff) throw std::runtime_error("Invalid Unicode path");
        appendUtf8(out, ch);
    }
    return out;
}
bool validPosixName(std::string_view name, size_t maximum) {
    return !name.empty() && name != "." && name != ".." && name.size() <= maximum && name.find('/') == name.npos && name.find('\0') == name.npos && validUtf8(name);
}
ResourceId::ResourceId(const std::filesystem::path& path) : local_(path) {}
ResourceId ResourceId::wsl(std::string distro, std::string path, std::string user) {
    if (distro.empty() || distro.find_first_of("/\\\r\n") != distro.npos || distro.find('\0') != distro.npos || !validUtf8(distro) || !validUtf8(user) || user.find('\0') != user.npos ||
        path.empty() || (path[0] != '/' && path != "~" && path.rfind("~/", 0) != 0) || path.find('\0') != path.npos || !validUtf8(path)) throw std::runtime_error("Enter a WSL distribution and an absolute Linux path (or ~/).");
    ResourceId result; result.authority_ = std::make_shared<ProjectLocation>(ProjectLocation{LocationKind::Wsl, std::move(distro), std::move(user), path}); result.posix_ = std::move(path); return result;
}
ResourceId ResourceId::parse(std::string_view saved) {
    if (saved.substr(0, 10) != "poppy-wsl:") return std::filesystem::u8path(saved);
    auto rest = saved.substr(10); const auto a = rest.find(':'); const auto b = a == rest.npos ? rest.npos : rest.find(':', a + 1);
    if (b == rest.npos) throw std::runtime_error("Invalid saved WSL location");
    return wsl(unhex(rest.substr(0, a)), unhex(rest.substr(b + 1)), unhex(rest.substr(a + 1, b - a - 1)));
}
ResourceId ResourceId::at(std::string path) const {
    if (!wsl()) return std::filesystem::u8path(path);
    if (!validUtf8(path) || path.find('\0') != path.npos) throw std::runtime_error("Unsupported Linux path encoding");
    ResourceId result; result.authority_ = authority_; result.posix_ = std::move(path); return result;
}
const ProjectLocation& ResourceId::location() const { static const ProjectLocation windows; return authority_ ? *authority_ : windows; }
const std::filesystem::path& ResourceId::local() const { if (wsl()) throw std::logic_error("A WSL resource is not a Windows path"); return local_; }
std::string ResourceId::u8string() const { return wsl() ? posix_ : local_.u8string(); }
std::string ResourceId::serialized() const { return wsl() ? "poppy-wsl:" + hex(location().distro) + ':' + hex(location().user) + ':' + hex(posix_) : u8string(); }
std::string ResourceId::key() const {
    if (wsl()) return "wsl:" + tuple(location().distro) + tuple(location().user) + tuple(posix_);
    auto text = resourceWide(local_.lexically_normal().make_preferred().u8string());
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return resourceUtf8(text);
}
std::wstring ResourceId::native() const { return resourceWide(u8string()); }
std::wstring ResourceId::generic_wstring() const { return resourceWide(generic_u8string()); }
std::string ResourceId::generic_u8string() const { return wsl() ? posix_ : local_.generic_u8string(); }
bool ResourceId::empty() const { return wsl() ? posix_.empty() : local_.empty(); }
bool ResourceId::is_absolute() const { return wsl() ? !posix_.empty() && posix_[0] == '/' : local_.is_absolute(); }
ResourceId ResourceId::filename() const { return wsl() ? at(posix_.substr(posix_.find_last_of('/') == posix_.npos ? 0 : posix_.find_last_of('/') + 1)) : ResourceId(local_.filename()); }
ResourceId ResourceId::parent_path() const {
    if (!wsl()) return local_.parent_path();
    const auto slash = posix_.find_last_of('/'); return at(slash == posix_.npos ? "" : slash == 0 ? "/" : posix_.substr(0, slash));
}
ResourceId ResourceId::root_path() const { return wsl() ? at(is_absolute() ? "/" : "") : ResourceId(local_.root_path()); }
ResourceId ResourceId::extension() const {
    if (!wsl()) return local_.extension();
    const auto name = filename().u8string(); const auto dot = name.find_last_of('.'); return at(dot == name.npos || dot == 0 ? "" : name.substr(dot));
}
ResourceId ResourceId::lexically_relative(const ResourceId& base) const {
    if (!sameAuthority(base)) return {};
    if (!wsl()) return local_.lexically_relative(base.local_);
    // Only split on '/', using Linux parsing even in the Windows build.
    auto parts = [](std::string_view s) { std::vector<std::string_view> out; size_t start = 0; for (size_t i = 0; i <= s.size(); ++i) if (i == s.size() || s[i] == '/') { if (i > start) out.push_back(s.substr(start, i - start)); start = i + 1; } return out; };
    const auto a = parts(posix_), b = parts(base.posix_); size_t common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common]) ++common;
    std::string out; for (size_t i = common; i < b.size(); ++i) out += "../";
    for (size_t i = common; i < a.size(); ++i) { if (!out.empty() && out.back() != '/') out += '/'; out += a[i]; }
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return at(out.empty() ? "." : out);
}
ResourceId ResourceId::lexically_normal() const { return wsl() ? *this : ResourceId(local_.lexically_normal()); }
ResourceId& ResourceId::make_preferred() { if (!wsl()) local_.make_preferred(); return *this; }
ResourceId ResourceId::operator/(std::string_view name) const { if (!wsl()) return local_ / std::filesystem::u8path(name); if (!name.empty() && name[0] == '/') return at(std::string(name)); return at(posix_ + (posix_.empty() || posix_.back() == '/' ? "" : "/") + std::string(name)); }
ResourceId ResourceId::operator/(const std::wstring& name) const { return *this / std::string_view(resourceUtf8(name)); }
ResourceId ResourceId::operator/(const std::filesystem::path& name) const { return *this / std::string_view(name.u8string()); }
bool ResourceId::sameAuthority(const ResourceId& other) const { return wsl() == other.wsl() && (!wsl() || (location().distro == other.location().distro && location().user == other.location().user)); }
bool ResourceId::contains(const ResourceId& other) const {
    if (!sameAuthority(other)) return false;
    const auto a = wsl() ? posix_ : key(), b = wsl() ? other.posix_ : other.key(); const auto separator = wsl() ? '/' : std::filesystem::path::preferred_separator;
    return a == b || (!a.empty() && b.size() > a.size() && b.compare(0, a.size(), a) == 0 && (a.back() == separator || b[a.size()] == separator));
}
bool ResourceId::operator==(const ResourceId& other) const { return sameAuthority(other) && (wsl() ? posix_ == other.posix_ : local_ == other.local_); }
bool ResourceId::operator==(const wchar_t* text) const { return native() == text; }
ResourceId relocateResource(const ResourceId& path, const ResourceId& source, const ResourceId& destination) {
    if (!source.contains(path)) return path;
    if (path.key() == source.key()) return destination;
    auto suffix = path.u8string().substr(source.u8string().size()); if (!suffix.empty() && (suffix[0] == '/' || (!source.wsl() && suffix[0] == '\\'))) suffix.erase(0, 1);
    return destination / std::string_view(suffix);
}
std::optional<ProjectLocation> wslUncLocation(std::string_view input) {
    std::string path(input); std::replace(path.begin(), path.end(), '\\', '/');
    if (path.rfind("//", 0) != 0) return {};
    const auto slash = path.find('/', 2); if (slash == path.npos) return {};
    auto host = path.substr(2, slash - 2); std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    if (host != "wsl$" && host != "wsl.localhost") return {};
    const auto root = path.find('/', slash + 1); const auto distro = path.substr(slash + 1, root == path.npos ? path.npos : root - slash - 1);
    if (distro.empty()) return {};
    return ProjectLocation{LocationKind::Wsl, distro, {}, root == path.npos ? "/" : path.substr(root)};
}
std::vector<std::string> parseWslDistributions(std::string_view bytes) {
    std::string text;
    if (bytes.size() >= 2 && ((static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) || bytes[1] == 0)) {
        size_t i = bytes[0] == '\xff' ? 2 : 0;
        if ((bytes.size() - i) % 2) throw std::runtime_error("Invalid UTF-16 WSL distribution list");
        for (; i < bytes.size(); i += 2) {
            char32_t ch = static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
            if (ch >= 0xd800 && ch <= 0xdbff) { if (i + 3 >= bytes.size()) throw std::runtime_error("Invalid WSL distribution name"); const unsigned low = static_cast<unsigned char>(bytes[i + 2]) | (static_cast<unsigned char>(bytes[i + 3]) << 8); if (low < 0xdc00 || low > 0xdfff) throw std::runtime_error("Invalid WSL distribution name"); ch = 0x10000 + ((ch - 0xd800) << 10) + low - 0xdc00; i += 2; }
            else if (ch >= 0xdc00 && ch <= 0xdfff) throw std::runtime_error("Invalid WSL distribution name");
            appendUtf8(text, ch);
        }
    } else text.assign(bytes);
    if (!validUtf8(text)) throw std::runtime_error("Invalid WSL distribution list encoding");
    std::vector<std::string> result; size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) if (i == text.size() || text[i] == '\r' || text[i] == '\n' || text[i] == '\0') { if (i > start) result.push_back(text.substr(start, i - start)); start = i + 1; }
    return result;
}
}
