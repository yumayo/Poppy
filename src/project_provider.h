#pragma once
#include "win.h"
#include "resource.h"
#include "file_search.h"
#include "content_search.h"
#include "git_repositories.h"
#include <memory>

namespace poppy {
struct ProviderEntry { ResourceId path; bool directory = false, reparse = false, unsupported = false; std::wstring label; };
struct ProviderListing { std::vector<ProviderEntry> entries; std::wstring error; bool truncated = false; };
struct ProviderFile { std::string bytes, version; std::wstring error; };
struct ProviderSave { std::string version; std::wstring error; };
enum class ConnectionState { Disconnected, Connecting, Ready, Unavailable };

// Methods run on the existing I/O/Git/index workers. State and revision are
// nonblocking snapshots that may be queried by the UI.
class ProjectProvider {
public:
    virtual ~ProjectProvider() = default;
    virtual ResourceId connect(const ResourceId& root) = 0;
    virtual ProviderListing listDirectory(const ResourceId& path) = 0;
    virtual ProviderFile readFile(const ResourceId& path) = 0;
    virtual ProviderSave saveFile(const ResourceId& path, const std::string& bytes, const std::string& version, const std::string& baseline) = 0;
    // Publish a new file only after writing it completely; never replace a name.
    virtual std::wstring createFile(const ResourceId& path, const std::string& bytes) = 0;
    virtual std::wstring renameResource(const ResourceId& source, const ResourceId& destination) = 0;
    virtual FileIndex indexFiles(const ResourceId& root, const std::function<bool()>& cancelled) = 0;
    virtual ContentSearchSummary searchContent(const ResourceId& root, const ContentSearchOptions& options,
        const ContentPublish& publish, const std::function<bool()>& cancelled) = 0;
    virtual ProcessResult git(const std::vector<std::wstring>& args, const ResourceId& directory, const std::atomic<bool>* cancelled) = 0;
    virtual GitRepositoryScan gitRepositories(const ResourceId& root, const std::function<bool()>& cancelled) = 0;
    virtual ConnectionState state() const { return ConnectionState::Ready; }
    virtual std::wstring error() const { return {}; }
    virtual uint64_t revision() const { return 0; }
    virtual void reconnect() {}
    virtual void expectIdentity(std::string) {}
    virtual std::string identity() const { return {}; }
    virtual std::vector<std::wstring> terminalCommand(const ResourceId&, const std::string&, const std::wstring& = {}) const { return {}; }
    virtual bool stopTerminal(const std::string&) const { return true; }
};
std::shared_ptr<ProjectProvider> createProjectProvider(const ResourceId& root);
std::vector<std::string> installedWslDistributions();
std::wstring wslExecutable();
std::filesystem::path windowsProcessDirectory();
}
