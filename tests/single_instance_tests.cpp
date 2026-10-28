#include "single_instance.h"
#include <array>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Predicate> void waitFor(Predicate predicate, const char* message) {
    const auto start = GetTickCount64();
    do { if (predicate()) return; Sleep(10); } while (GetTickCount64() - start < 10000);
    throw std::runtime_error(message);
}
struct Fixture {
    fs::path root = fs::temp_directory_path() / (L"Poppy startup 日本語 " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
    Fixture() { check(fs::create_directory(root), "Cannot create startup test directory"); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    fs::path folder(const wchar_t* name) { const auto path = (root / name).make_preferred(); fs::create_directories(path); return path; }
};
struct AppDataEnvironment {
    std::wstring previous;
    bool existed = false;
    explicit AppDataEnvironment(const fs::path& directory) {
        const DWORD size = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
        if (size) {
            previous.resize(size); const auto count = GetEnvironmentVariableW(L"APPDATA", previous.data(), size);
            check(count && count < size, "Cannot read APPDATA"); previous.resize(count); existed = true;
        }
        check(SetEnvironmentVariableW(L"APPDATA", directory.c_str()) != FALSE, "Cannot isolate startup test storage");
    }
    ~AppDataEnvironment() { SetEnvironmentVariableW(L"APPDATA", existed ? previous.c_str() : nullptr); }
};
HWND processWindow(DWORD process, const wchar_t* className = L"PoppyMain") {
    struct Search { DWORD process; const wchar_t* className; HWND result = nullptr; } search{process, className};
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(context);
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        wchar_t name[64]{}; GetClassNameW(window, name, 64);
        if (process != search.process || std::wstring(name) != search.className) return TRUE;
        search.result = window; return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}
struct Application {
    PROCESS_INFORMATION process{};
    Application(const fs::path& executable, const std::vector<std::wstring>& arguments = {}, const fs::path& directory = {}) {
        std::vector<std::wstring> args{executable.native()}; args.insert(args.end(), arguments.begin(), arguments.end());
        auto command = poppy::processCommandLine(args);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            directory.empty() ? nullptr : directory.c_str(), &startup, &process) != FALSE, "Cannot start Poppy");
        CloseHandle(process.hThread);
    }
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    ~Application() {
        if (auto window = processWindow(process.dwProcessId)) PostMessageW(window, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000);
        }
        CloseHandle(process.hProcess);
    }
    HWND window() const { return processWindow(process.dwProcessId); }
    void exited(DWORD expected = 0) const {
        check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "The secondary Poppy did not exit");
        DWORD code = 0; check(GetExitCodeProcess(process.hProcess, &code) && code == expected, "Poppy exited with an unexpected code");
        check(!window(), "A secondary Poppy left another main window");
    }
};
size_t projects(HWND window) {
    size_t count = 0;
    for (HWND project = nullptr; (project = FindWindowExW(window, project, L"PoppyProject", nullptr));) ++count;
    return count;
}
HWND waitProject(const Application& app, const fs::path& path, size_t count) {
    HWND window = nullptr;
    waitFor([&] {
        window = app.window();
        wchar_t title[32768]{};
        return window && IsWindowVisible(window) && projects(window) == count &&
            GetWindowTextW(window, title, 32768) && std::wstring(title) == L"Poppy — " + path.native();
    }, "The running Poppy did not add and select the requested project");
    return window;
}
HWND activeSurface(HWND window) {
    for (HWND project = nullptr; (project = FindWindowExW(window, project, L"PoppyProject", nullptr));)
        if (IsWindowVisible(project)) return project;
    return nullptr;
}
LRESULT copyData(HWND window, COPYDATASTRUCT* data) {
    DWORD_PTR result = FALSE;
    check(SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(data),
        SMTO_BLOCK | SMTO_ABORTIFHUNG, 1000, &result) != 0, "Poppy did not respond to a startup message");
    return static_cast<LRESULT>(result);
}
void forwarding(const fs::path& executable, Fixture& fixture) {
    const auto a = fixture.folder(L"first 日本語"), b = fixture.folder(L"second=project"),
        c = fixture.folder(L"caller/relative 日本語"), d = fixture.folder(L"fourth"), e = fixture.folder(L"fifth");
    {
        Application app(executable, {L"--project", a.native()});
        const auto host = waitProject(app, a, 1), surface = activeSurface(host);
        check(surface != nullptr, "The first project has no surface");
        { Application second(executable, {L"--project=" + b.native()}); second.exited(); }
        check(waitProject(app, b, 2) == host, "Forwarding replaced the main window");
        { Application duplicate(executable, {L"--project", (a / L".").native()}); duplicate.exited(); }
        waitProject(app, a, 2);
        check(activeSurface(host) == surface, "A duplicate request rebuilt the project");
        { Application relative(executable, {L"--project", L"relative 日本語"}, c.parent_path()); relative.exited(); }
        waitProject(app, c, 3);
        { Application multiple(executable, {L"--project", a.native(), L"--project=" + d.native()}); multiple.exited(); }
        waitProject(app, d, 4);
        ShowWindow(host, SW_MINIMIZE);
        check(IsIconic(host) != FALSE, "Cannot minimize Poppy for the activation test");
        { Application activate(executable); activate.exited(); }
        waitFor([&] { return !IsIconic(host); }, "A no-argument launch did not restore the existing window");
        check(projects(host) == 4, "Activation changed the open projects");
        {
            AppDataEnvironment separate(fixture.folder(L"other profile"));
            Application isolated(executable, {e.native()});
            const auto other = waitProject(isolated, e, 1);
            { Application second(executable, {L"--project", e.native()}); second.exited(); }
            check(other != host && projects(host) == 4 && projects(other) == 1, "Startup requests crossed storage profiles");
        }
        const auto reject = [&](std::wstring text) {
            COPYDATASTRUCT data{poppy::startupCopyData, static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)), text.data()};
            check(!copyData(host, &data), "An invalid startup message was accepted");
        };
        reject(L"Poppy.exe --unknown=value"); reject(L"Poppy.exe --project relative");
        reject(std::wstring(L"Poppy.exe\0--project X", 21));
        std::wstring unterminated = L"Poppy.exe";
        COPYDATASTRUCT bad{poppy::startupCopyData, static_cast<DWORD>(unterminated.size() * sizeof(wchar_t)), unterminated.data()};
        check(!copyData(host, &bad), "An unterminated startup message was accepted");
        check(projects(host) == 4, "Rejected messages changed the open projects");
        {
            Application invalid(executable, {L"--project", (fixture.root / L"missing").native()});
            HWND dialog = nullptr;
            waitFor([&] { dialog = processWindow(invalid.process.dwProcessId, L"#32770"); return dialog != nullptr; }, "Missing directory did not report an error");
            PostMessageW(dialog, WM_COMMAND, IDOK, 0); invalid.exited(1);
            check(projects(host) == 4, "A missing directory was added to Projects");
        }
    }
    {
        Application restored(executable);
        waitProject(restored, d, 4);
    }
    {
        Application restored(executable, {L"--project", e.native()});
        waitProject(restored, e, 5);
    }
}
void simultaneous(const fs::path& executable, Fixture& fixture) {
    AppDataEnvironment environment(fixture.folder(L"simultaneous profile"));
    const auto a = fixture.folder(L"rapid A"), b = fixture.folder(L"rapid B"), c = fixture.folder(L"rapid C");
    Application first(executable, {L"--project", a.native()});
    Application second(executable, {L"--project", b.native()});
    Application third(executable, {L"--project", c.native()});
    const std::array<Application*, 3> apps{&first, &second, &third};
    Application* owner = nullptr;
    waitFor([&] {
        size_t live = 0, windows = 0;
        for (auto app : apps) {
            if (WaitForSingleObject(app->process.hProcess, 0) == WAIT_TIMEOUT) ++live;
            if (const auto window = app->window()) { ++windows; if (IsWindowVisible(window) && projects(window) == 3) owner = app; }
        }
        return owner && live == 1 && windows == 1;
    }, "Simultaneous startup lost projects or left more than one Poppy running");
    for (auto app : apps) if (app != owner) app->exited();
    for (const auto& path : {a, b, c}) {
        Application duplicate(executable, {L"--project", path.native()}); duplicate.exited();
        waitProject(*owner, path, 3);
    }
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "Pass the path to Poppy.exe");
        Fixture fixture; AppDataEnvironment environment(fixture.folder(L"profile"));
        const auto executable = fs::absolute(argv[1]);
        forwarding(executable, fixture); simultaneous(executable, fixture);
        std::cout << "Startup forwarding, duplicate, relative path, activation, profile, persistence and simultaneous startup tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
