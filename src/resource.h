#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
enum class LocationKind { Windows, Wsl };
struct ProjectLocation {
    LocationKind kind = LocationKind::Windows;
    std::string distro, user, root;
};

// A Linux path is never parsed by std::filesystem on Windows. Resources share
// their connection authority; POSIX names (including backslashes) stay UTF-8.
class ResourceId {
public:
    ResourceId() = default;
    ResourceId(const std::filesystem::path& path);
    ResourceId(const wchar_t* path) : ResourceId(std::filesystem::path(path)) {}
    static ResourceId wsl(std::string distro, std::string path, std::string user = {});
    static ResourceId parse(std::string_view saved);
    ResourceId at(std::string path) const;
    bool wsl() const { return authority_ && authority_->kind == LocationKind::Wsl; }
    const ProjectLocation& location() const;
    const std::filesystem::path& local() const;
    std::string u8string() const;
    std::string serialized() const;
    std::string key() const;
    std::wstring native() const;
    std::wstring generic_wstring() const;
    std::string generic_u8string() const;
    bool empty() const;
    bool is_absolute() const;
    void clear() { *this = {}; }
    ResourceId filename() const;
    ResourceId parent_path() const;
    ResourceId root_path() const;
    ResourceId extension() const;
    ResourceId lexically_relative(const ResourceId& base) const;
    ResourceId lexically_normal() const;
    ResourceId& make_preferred();
    ResourceId operator/(std::string_view name) const;
    ResourceId operator/(const std::wstring& name) const;
    ResourceId operator/(const std::filesystem::path& name) const;
    bool sameAuthority(const ResourceId& other) const;
    bool contains(const ResourceId& other) const;
    bool operator==(const ResourceId& other) const;
    bool operator!=(const ResourceId& other) const { return !(*this == other); }
    bool operator==(const wchar_t* text) const;
private:
    std::shared_ptr<const ProjectLocation> authority_;
    std::string posix_;
    std::filesystem::path local_;
};
ResourceId relocateResource(const ResourceId& path, const ResourceId& source, const ResourceId& destination);
// Recognize only WSL's two UNC hosts. SMB shares keep Windows semantics.
std::optional<ProjectLocation> wslUncLocation(std::string_view path);
std::vector<std::string> parseWslDistributions(std::string_view bytes);
bool validUtf8(std::string_view bytes);
std::wstring resourceWide(std::string_view text);
std::string resourceUtf8(std::wstring_view text);
bool validPosixName(std::string_view name, size_t nameMax = 255);
}
