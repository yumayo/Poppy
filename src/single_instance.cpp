#include "single_instance.h"
#include "session.h"
#include <shellapi.h>
#include <memory>
#include <stdexcept>

namespace poppy {
StartupOptions parseStartupCommandLine(const wchar_t* commandLine) {
    int count = 0;
    auto arguments = CommandLineToArgvW(commandLine, &count);
    if (!arguments) throw std::runtime_error("Cannot read the command line.");
    std::unique_ptr<void, decltype(&LocalFree)> memory(arguments, &LocalFree);
    return parseStartupOptions(std::vector<std::wstring>(arguments + 1, arguments + count));
}
SingleInstance::SingleInstance(const fs::path& directory) {
    name_ = L"Local\\Poppy.Workspace." + wide(projectPathHash(utf8(pathKey(fs::absolute(directory)))));
    mutex_.reset(CreateMutexW(nullptr, FALSE, name_.c_str()));
    if (!mutex_) throw std::runtime_error("Cannot create the Poppy instance lock: " + utf8(winError()));
}
SingleInstance::~SingleInstance() { if (owned_) ReleaseMutex(mutex_.value); }
bool SingleInstance::acquireOrForward(const StartupOptions& options) {
    std::vector<std::wstring> arguments{L"Poppy.exe"};
    for (const auto& project : options.projects) { arguments.push_back(L"--project"); arguments.push_back(project); }
    const auto command = processCommandLine(arguments);
    const auto bytes = (command.size() + 1) * sizeof(wchar_t);
    if (bytes > 65536) throw std::runtime_error("The startup request exceeds the 64 KiB limit.");
    const auto start = GetTickCount64();
    do {
        const DWORD lock = WaitForSingleObject(mutex_.value, 0);
        if (lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED) { owned_ = true; return true; }
        if (lock != WAIT_TIMEOUT) throw std::runtime_error("Cannot acquire the Poppy instance lock.");
        for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, L"PoppyMain", nullptr));) {
            if (!GetPropW(window, name_.c_str())) continue;
            DWORD process = 0; GetWindowThreadProcessId(window, &process);
            AllowSetForegroundWindow(process);
            COPYDATASTRUCT data{startupCopyData, static_cast<DWORD>(bytes), const_cast<wchar_t*>(command.c_str())};
            DWORD_PTR accepted = FALSE;
            if (!SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 5000, &accepted))
                throw std::runtime_error("The running Poppy did not respond to the startup request. Please try again when it is ready.");
            if (!accepted) throw std::runtime_error("The running Poppy could not accept the startup request. It may be closing.");
            return false;
        }
        // The owner may still be creating its window. Retry the lock as well,
        // so a failed or crashed startup does not leave a permanent blocker.
        Sleep(25);
    } while (GetTickCount64() - start < 10000);
    throw std::runtime_error("The running Poppy has not finished starting. Please try again when it is ready.");
}
void SingleInstance::publish(HWND window) {
    if (!SetPropW(window, name_.c_str(), reinterpret_cast<HANDLE>(1)))
        throw std::runtime_error("Cannot publish the Poppy instance window.");
}
void SingleInstance::unpublish(HWND window) { RemovePropW(window, name_.c_str()); }
}
