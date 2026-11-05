#include "win.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    fs::path root = fs::temp_directory_path() / (L"PoppyLauncher 日本語-" + std::to_wstring(GetCurrentProcessId()));
    Fixture() { fs::remove_all(root); fs::create_directories(root / "caller"); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};
struct Environment {
    std::wstring name, previous;
    explicit Environment(std::wstring key, const std::wstring& value) : name(std::move(key)) {
        const auto size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (size) { previous.resize(size); previous.resize(GetEnvironmentVariableW(name.c_str(), previous.data(), size)); }
        check(SetEnvironmentVariableW(name.c_str(), value.c_str()) != FALSE, "Cannot set fixture environment");
    }
    ~Environment() { SetEnvironmentVariableW(name.c_str(), previous.empty() ? nullptr : previous.c_str()); }
};
struct Process {
    poppy::Handle process;
    Process(const fs::path& executable, const fs::path& directory, const fs::path& record, const std::wstring& stop) {
        Environment recordVariable(L"POPPY_TEST_RECORD", record.native()), stopVariable(L"POPPY_TEST_STOP", stop);
        auto command = poppy::processCommandLine({executable.native(), L"--project", L"relative 日本語 project"});
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            directory.c_str(), &startup, &info) != FALSE, "Cannot launch fixture");
        process.reset(info.hProcess); CloseHandle(info.hThread);
    }
    ~Process() { if (WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) { TerminateProcess(process.value, 105); WaitForSingleObject(process.value, 5000); } }
    void exitCode(DWORD expected) {
        check(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0, "Launcher did not exit");
        DWORD code = 0;
        check(GetExitCodeProcess(process.value, &code) && code == expected, "Launcher ran the wrong release or lost its resources");
    }
};
std::wstring waitRecord(const fs::path& path, HANDLE process) {
    const auto start = GetTickCount64();
    do {
        std::ifstream input(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        std::wstring contents(bytes.size() / sizeof(wchar_t), L'\0');
        if (bytes.size() % sizeof(wchar_t) == 0 && !bytes.empty()) std::memcpy(contents.data(), bytes.data(), bytes.size());
        constexpr std::wstring_view ready = L"\nREADY\n";
        if (contents.size() >= ready.size() && contents.compare(contents.size() - ready.size(), ready.size(), ready) == 0) return contents;
        check(WaitForSingleObject(process, 0) == WAIT_TIMEOUT, "Fixture failed to initialize");
        Sleep(10);
    } while (GetTickCount64() - start < 5000);
    throw std::runtime_error("Fixture did not write its startup record");
}
void select(const fs::path& root, char id) {
    { std::ofstream file(root / "current.next"); file << std::string(64, id) << '\n'; check(static_cast<bool>(file), "Cannot write release pointer"); }
    check(MoveFileExW((root / "current.next").c_str(), (root / "current.txt").c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE,
        "Cannot switch the release while Poppy is running");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4, "Expected launcher and two fixture DLLs");
        Fixture fixture;
        const auto executable = fixture.root / "Poppy.exe", caller = fixture.root / "caller";
        fs::copy_file(argv[1], executable);
        for (int index = 0; index < 2; ++index) {
            const auto release = fixture.root / "releases" / std::string(64, static_cast<char>('a' + index));
            fs::create_directories(release);
            fs::copy_file(argv[2 + index], release / "PoppyApp.dll");
            std::ofstream(release / "version.txt") << 11 * (index + 1);
        }
        const auto eventName = L"Local\\PoppyLauncherTest-" + std::to_wstring(GetCurrentProcessId());
        poppy::Handle stop(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
        check(static_cast<bool>(stop), "Cannot create fixture stop event");
        select(fixture.root, 'a');
        Process first(executable, caller, fixture.root / "first.txt", eventName);
        const auto before = waitRecord(fixture.root / "first.txt", first.process.value);
        check(before.find(L"releases\\" + std::wstring(64, L'a') + L"\\PoppyApp.dll") != std::wstring::npos,
            "The first launch used the wrong DLL");
        check(before.find(L"--project \"relative 日本語 project\"") != std::wstring::npos && before.find(caller.native()) != std::wstring::npos,
            "The launcher changed arguments or the caller's working directory");
        select(fixture.root, 'b');
        Process second(executable, caller, fixture.root / "second.txt", eventName);
        const auto after = waitRecord(fixture.root / "second.txt", second.process.value);
        check(after.find(L"releases\\" + std::wstring(64, L'b') + L"\\PoppyApp.dll") != std::wstring::npos,
            "The next launch did not select the new DLL");
        check(WaitForSingleObject(first.process.value, 0) == WAIT_TIMEOUT, "Updating interrupted the running release");
        SetEvent(stop.value);
        first.exitCode(11); second.exitCode(22);
        std::cout << "Launcher update tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
