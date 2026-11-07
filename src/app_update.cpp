#include "app_update.h"
#include "release_id.h"
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace poppy {
bool applicationUpdateAvailable(const std::filesystem::path& installation, const std::filesystem::path& runningRelease) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::equivalent(installation / "releases", runningRelease.parent_path(), error) || error) return false;
    try {
        const auto running = parseReleaseId(runningRelease.filename().u8string());
        char bytes[67]{};
        size_t size = 0;
#ifdef _WIN32
        // Publishing atomically replaces this file; never hold up that rename.
        const auto file = CreateFileW((installation / "current.txt").c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD read = 0;
        const bool success = ReadFile(file, bytes, sizeof(bytes), &read, nullptr) != FALSE;
        CloseHandle(file);
        if (!success) return false;
        size = read;
#else
        std::ifstream file(installation / "current.txt", std::ios::binary);
        if (!file) return false;
        file.read(bytes, sizeof(bytes));
        if (file.bad()) return false;
        size = static_cast<size_t>(file.gcount());
#endif
        const auto selected = parseReleaseId(std::string_view(bytes, size));
        return selected != running && fs::is_regular_file(installation / "releases" / selected / "PoppyApp.dll", error) && !error;
    } catch (const std::invalid_argument&) { return false; }
}
}
