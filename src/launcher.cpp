#include "launcher_api.h"
#include "release_id.h"
#include <cstring>
#include <filesystem>
#include <string>

namespace {
[[noreturn]] void fail(const wchar_t* action, DWORD code = GetLastError()) {
    wchar_t* detail = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&detail), 0, nullptr);
    std::wstring message = action;
    message += L"\n\n";
    message += detail ? detail : L"Unknown Windows error.";
    if (detail) LocalFree(detail);
    throw message;
}
std::filesystem::path releaseDirectory() {
    std::wstring executable(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!count || count >= executable.size()) fail(L"Cannot locate Poppy.exe.");
    executable.resize(count);
    const auto root = std::filesystem::path(executable).parent_path();
    // Allow an updater to replace the pointer while this launch reads its snapshot.
    const auto file = CreateFileW((root / L"current.txt").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) fail(L"Cannot open current.txt. Publish a Poppy release first.");
    char bytes[67]{};
    DWORD read = 0;
    const auto success = ReadFile(file, bytes, sizeof(bytes), &read, nullptr);
    const auto error = GetLastError();
    CloseHandle(file);
    if (!success) fail(L"Cannot read current.txt.", error);
    const auto id = poppy::parseReleaseId(std::string_view(bytes, read));
    return root / L"releases" / std::wstring(id.begin(), id.end());
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show) {
    try {
        const auto library = releaseDirectory() / L"PoppyApp.dll";
        const auto module = LoadLibraryExW(library.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) fail(L"Cannot load PoppyApp.dll from the selected release. Publish the release again.");
        const auto address = GetProcAddress(module, poppyEntryName);
        if (!address) fail(L"This release does not provide PoppyMainV1. Use a compatible Poppy release.");
        PoppyEntryV1 entry = nullptr;
        static_assert(sizeof(entry) == sizeof(address));
        std::memcpy(&entry, &address, sizeof(entry));
        // Keep the DLL mapped until process teardown, including its static objects
        // and any pending WebView2 callbacks. The DLL owns the message loop.
        return entry(module, show);
    } catch (const std::wstring& message) {
        MessageBoxW(nullptr, message.c_str(), L"Poppy", MB_OK | MB_ICONERROR);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Poppy", MB_OK | MB_ICONERROR);
    }
    return 1;
}
