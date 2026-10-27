#include "project_provider.h"
#include "text.h"
#include "hook_server.h"
#include "wsl_protocol.h"
#include <algorithm>

namespace poppy {
std::shared_ptr<ProjectProvider> createWslProvider(const ResourceId& root);
namespace {
class WindowsProvider final : public ProjectProvider {
public:
    ResourceId connect(const ResourceId& root) override { return root; }
    ProviderListing listDirectory(const ResourceId& path) override {
        ProviderListing result; std::error_code error;
        fs::directory_iterator iterator(path.local(), fs::directory_options::skip_permission_denied, error), end;
        if (error) { result.error = wide(error.message()); return result; }
        for (; iterator != end; iterator.increment(error)) {
            if (error) { result.error = wide(error.message()); break; }
            const auto& entry = *iterator;
            if (result.entries.size() == 10000) { result.truncated = true; break; }
            const DWORD attributes = GetFileAttributesW(entry.path().c_str()); if (attributes == INVALID_FILE_ATTRIBUTES) continue;
            result.entries.push_back({entry.path(), (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0, false, {}});
        }
        return result;
    }
    ProviderFile readFile(const ResourceId& path) override {
        ProviderFile result;
        Handle file(CreateFileW(path.local().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file) { result.error = winError(); return result; }
        LARGE_INTEGER size{}; if (!GetFileSizeEx(file.value, &size)) { result.error = winError(); return result; }
        if (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(maxFileBytes)) { result.error = L"This file exceeds the 16 MiB preview limit."; return result; }
        result.bytes.resize(static_cast<size_t>(size.QuadPart)); size_t offset = 0;
        while (offset < result.bytes.size()) { DWORD count = 0; if (!ReadFile(file.value, result.bytes.data() + offset, static_cast<DWORD>(result.bytes.size() - offset), &count, nullptr)) { result.error = winError(); return result; } if (!count) break; offset += count; }
        result.bytes.resize(offset); return result;
    }
    ProviderSave saveFile(const ResourceId& path, const std::string& bytes, const std::string&, const std::string& baseline) override {
        if (bytes.size() > maxFileBytes) return {{}, L"Save exceeds the 16 MiB limit."};
        Handle original(CreateFileW(path.local().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!original) return {{}, winError()};
        LARGE_INTEGER size{}; if (!GetFileSizeEx(original.value, &size)) return {{}, winError()};
        const std::wstring conflict = L"The file changed outside Poppy. Your edits have been kept. Reconcile the external changes before saving.";
        if (size.QuadPart != static_cast<LONGLONG>(baseline.size())) return {{}, conflict};
        std::string current(baseline.size(), 0); size_t offset = 0;
        while (offset < current.size()) { DWORD count = 0; if (!ReadFile(original.value, current.data() + offset, static_cast<DWORD>(current.size() - offset), &count, nullptr)) return {{}, winError()}; if (!count) break; offset += count; }
        if (offset != baseline.size() || current != baseline) return {{}, conflict};
        auto temporary = path.local(); temporary += L".poppy-" + wide(newSessionId()) + L".tmp";
        Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)); if (!output) return {{}, winError()};
        std::wstring error; offset = 0;
        while (offset < bytes.size()) { DWORD count = 0; if (!WriteFile(output.value, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr) || !count) { error = winError(); break; } offset += count; }
        if (error.empty() && !FlushFileBuffers(output.value)) error = winError();
        output.reset();
        if (error.empty() && !ReplaceFileW(path.local().c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)) error = winError();
        if (!error.empty()) DeleteFileW(temporary.c_str());
        return {{}, error};
    }
    std::wstring createFile(const ResourceId& path, const std::string& bytes) override {
        if (bytes.size() > maxFileBytes) return L"New file exceeds the 16 MiB limit.";
        const auto temporary = path.local().parent_path() / (L".poppy-paste-" + wide(newSessionId()) + L".tmp");
        Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output) return winError();
        std::wstring error; size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD count = 0;
            if (!WriteFile(output.value, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr) || !count) { error = winError(); break; }
            offset += count;
        }
        if (error.empty() && !FlushFileBuffers(output.value)) error = winError();
        output.reset();
        if (error.empty() && !MoveFileExW(temporary.c_str(), path.local().c_str(), MOVEFILE_WRITE_THROUGH)) error = winError();
        if (!error.empty()) DeleteFileW(temporary.c_str());
        return error;
    }
    std::wstring createDirectory(const ResourceId& path) override { return CreateDirectoryW(path.local().c_str(), nullptr) ? std::wstring{} : winError(); }
    std::wstring renameResource(const ResourceId& source, const ResourceId& destination) override { return MoveFileExW(source.local().c_str(), destination.local().c_str(), MOVEFILE_WRITE_THROUGH) ? std::wstring{} : winError(); }
    FileIndex indexFiles(const ResourceId& root, const std::function<bool()>& cancelled) override { return poppy::indexFiles(root.local(), cancelled); }
    ContentSearchSummary searchContent(const ResourceId& root, const ContentSearchOptions& options,
        const ContentPublish& publish, const std::function<bool()>& cancelled) override { return searchContents(root.local(), options, publish, cancelled); }
    ProcessResult git(const std::vector<std::wstring>& args, const ResourceId& directory, const std::atomic<bool>* cancelled) override { return runProcess(args, directory.local(), 5000, cancelled); }
    GitRepositoryScan gitRepositories(const ResourceId& root, const std::function<bool()>& cancelled) override { return scanGitRepositories(root.local(), cancelled); }
};
}
std::shared_ptr<ProjectProvider> createProjectProvider(const ResourceId& root) { return root.wsl() ? createWslProvider(root) : std::make_shared<WindowsProvider>(); }
std::wstring wslExecutable() { wchar_t system[MAX_PATH]{}; if (!GetSystemDirectoryW(system, MAX_PATH)) throw std::runtime_error("Cannot locate wsl.exe"); return std::wstring(system) + L"\\wsl.exe"; }
fs::path windowsProcessDirectory() { wchar_t system[MAX_PATH]{}; if (!GetSystemDirectoryW(system, MAX_PATH)) throw std::runtime_error("Cannot locate Windows system directory"); return system; }
std::vector<std::string> installedWslDistributions() {
    const auto result = runProcess({wslExecutable(), L"--list", L"--quiet"}, windowsProcessDirectory(), 10000);
    if (result.code) throw std::runtime_error("Cannot list WSL distributions (exit code " + std::to_string(result.code) + ").\n\n" + utf8(result.error));
    auto distributions = parseWslDistributions(result.output); if (distributions.empty()) throw std::runtime_error("No WSL distributions are registered. Install a distribution and retry."); return distributions;
}
}
