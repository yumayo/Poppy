#pragma once
#include "session.h"
#include <filesystem>
#include <memory>
#include <optional>

namespace poppy {
// All strings crossing the storage boundary are UTF-8. Each project owns a
// storage.sql below projects/; the app database owns the directory registry.
class Storage {
public:
    explicit Storage(std::filesystem::path directory);
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    AppSession loadApp();
    // The registry outlives open_projects, including projects closed by the user.
    std::vector<std::string> projectHistory();
    void saveApp(const AppSession& state);
    std::optional<ProjectSession> loadProject(const std::string& key, const std::string& path);
    void saveProject(const std::string& key, const std::string& path, const ProjectSession& state);
    std::filesystem::path projectDirectory(const std::string& key, const std::string& path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
