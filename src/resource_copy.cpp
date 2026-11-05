#include "resource_copy.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace poppy {
namespace fs = std::filesystem;
namespace {
void copyLastWriteTime(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
    // MinGW's filesystem setter uses _wutime, which cannot open directories.
    // Native attribute access supports both folders and files without rounding.
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &attributes))
        throw std::system_error(GetLastError(), std::system_category(), "Cannot read copy source timestamps");
    const auto handle = CreateFileW(destination.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "Cannot open copy destination to set timestamps");
    const auto error = SetFileTime(handle, nullptr, nullptr, &attributes.ftLastWriteTime) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (error) throw std::system_error(error, std::system_category(), "Cannot preserve copied timestamps");
#else
    fs::last_write_time(destination, fs::last_write_time(source));
#endif
}
}
void copyResourcePath(const fs::path& source, const fs::path& destination, bool rejectLinks) {
    if (!source.is_absolute() || !destination.is_absolute() || destination.filename().empty())
        throw std::runtime_error("Copy requires absolute file or folder paths.");
    if (fs::exists(fs::symlink_status(destination))) throw std::runtime_error("The destination already exists.");
    if (!fs::is_directory(fs::symlink_status(destination.parent_path())))
        throw std::runtime_error("The destination is not a folder or is a link.");
#ifdef _WIN32
    if (GetFileAttributesW(destination.parent_path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::runtime_error("Cannot paste into a link or junction.");
#endif
    if (fs::is_directory(fs::symlink_status(source))) {
        const auto from = fs::canonical(source), to = fs::canonical(destination.parent_path());
        // equivalent() also handles Windows case folding and directory aliases.
        for (auto path = to; !path.empty(); path = path.parent_path()) {
            if (fs::equivalent(from, path)) throw std::runtime_error("Cannot copy a folder into itself or its descendants.");
            if (path == path.parent_path()) break;
        }
    }
    static std::atomic<unsigned> sequence{0};
    fs::path staging;
    for (;;) {
        staging = destination.parent_path() / (".poppy-copy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + '-' + std::to_string(sequence++));
        if (fs::create_directory(staging)) break;
    }
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            // Copied read-only directories/files must not prevent cleanup.
            std::vector<fs::path> pending{path};
            std::error_code error;
            while (!pending.empty()) {
                const auto current = std::move(pending.back()); pending.pop_back();
                const auto status = fs::symlink_status(current, error);
                if (error || fs::is_symlink(status)) continue;
                fs::permissions(current, fs::perms::owner_all, fs::perm_options::add, error);
                if (fs::is_directory(status)) for (fs::directory_iterator it(current, error), end; !error && it != end; it.increment(error)) pending.push_back(it->path());
            }
            fs::remove_all(path, error);
        }
    } cleanup{staging};
    const auto content = staging / "content";
    struct Pending { fs::path source, target; };
    std::vector<Pending> pending{{source, content}}, directories;
    while (!pending.empty()) {
        const auto item = std::move(pending.back()); pending.pop_back();
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(item.source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) throw std::system_error(GetLastError(), std::system_category(), "Cannot inspect copy source");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Copying links or junctions is not supported on Windows.");
#endif
        const auto status = fs::symlink_status(item.source);
        if (fs::is_symlink(status)) {
            if (rejectLinks) throw std::runtime_error("Copying Windows links or junctions into WSL is not supported.");
            fs::copy_symlink(item.source, item.target);
        }
        else if (fs::is_directory(status)) {
            fs::create_directory(item.target);
            directories.push_back(item);
            for (const auto& entry : fs::directory_iterator(item.source)) pending.push_back({entry.path(), item.target / entry.path().filename()});
        } else if (fs::is_regular_file(status)) {
            fs::copy_file(item.source, item.target);
            copyLastWriteTime(item.source, item.target);
        } else throw std::runtime_error("Only regular files, folders and symbolic links can be copied.");
    }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
        copyLastWriteTime(it->source, it->target);
        fs::permissions(it->target, fs::status(it->source).permissions());
    }
#ifdef _WIN32
    if (!MoveFileExW(content.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::system_error(GetLastError(), std::system_category(), "Cannot copy without overwriting");
#else
    if (syscall(SYS_renameat2, AT_FDCWD, content.c_str(), AT_FDCWD, destination.c_str(), 1 /* RENAME_NOREPLACE */))
        throw std::system_error(errno, std::generic_category(), "Cannot copy without overwriting");
#endif
}
}
