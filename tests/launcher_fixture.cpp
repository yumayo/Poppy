#include "launcher_api.h"
#include <filesystem>
#include <fstream>

extern "C" __declspec(dllexport) int WINAPI PoppyMainV1(HINSTANCE module, int) {
    wchar_t path[32768]{}, record[32768]{}, stop[256]{}, directory[32768]{};
    if (!GetModuleFileNameW(module, path, 32768) ||
        !GetEnvironmentVariableW(L"POPPY_TEST_RECORD", record, 32768) ||
        !GetEnvironmentVariableW(L"POPPY_TEST_STOP", stop, 256) ||
        !GetCurrentDirectoryW(32768, directory)) return 100;
    const auto event = OpenEventW(SYNCHRONIZE, FALSE, stop);
    if (!event) return 101;
    {
        std::ofstream output(std::filesystem::path(record), std::ios::binary);
        const auto contents = std::to_wstring(POPPY_FIXTURE_VERSION) + L"\n" + path + L"\n" + GetCommandLineW() + L"\n" + directory + L"\nREADY\n";
        output.write(reinterpret_cast<const char*>(contents.data()), static_cast<std::streamsize>(contents.size() * sizeof(wchar_t)));
        if (!output) { CloseHandle(event); return 102; }
    }
    const auto wait = WaitForSingleObject(event, 20000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0) return 103;
    // Check a release-local resource after current.txt has changed.
    std::ifstream resource(std::filesystem::path(path).parent_path() / "version.txt");
    int version = 0; resource >> version;
    return version == POPPY_FIXTURE_VERSION ? version : 104;
}
