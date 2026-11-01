#include "win.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace poppy;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
fs::path executable() {
    std::wstring path(32768, L'\0'); const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    check(count && count < path.size(), "Cannot locate the lifetime fixture"); path.resize(count); return path;
}
struct Child {
    Handle process;
    Child(const std::vector<std::wstring>& args, ProcessJob* job = nullptr) {
        auto line = processCommandLine(args); STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
        check(CreateProcessW(args.front().c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
              nullptr, nullptr, &startup, &info) != FALSE, "Cannot start a lifetime fixture");
        process.reset(info.hProcess); Handle thread(info.hThread);
        if (job && !AssignProcessToJobObject(job->handle(), process.value)) {
            TerminateProcess(process.value, 1); WaitForSingleObject(process.value, INFINITE);
            throw std::runtime_error("Cannot assign a lifetime fixture to its job");
        }
        check(ResumeThread(thread.value) != static_cast<DWORD>(-1), "Cannot resume a lifetime fixture");
    }
    ~Child() { if (process && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) { TerminateProcess(process.value, 1); WaitForSingleObject(process.value, INFINITE); } }
};
int fixture(const std::wstring& mode, const fs::path& file, const std::wstring& event) {
    if (mode == L"--hold") {
        Handle lock(CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        check(static_cast<bool>(lock), "Cannot acquire the conversation-like lock");
        Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, event.c_str())); check(ready && SetEvent(ready.value), "Cannot signal the leaf fixture");
        Sleep(INFINITE); return 0;
    }
    const auto childEvent = event + L"-child-" + std::to_wstring(GetCurrentProcessId());
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, childEvent.c_str()));
    Child leaf({executable().native(), L"--hold", file.native(), childEvent});
    check(WaitForSingleObject(ready.value, 10000) == WAIT_OBJECT_0, "The descendant did not acquire its lock");
    const auto pid = GetProcessId(leaf.process.value);
    auto pidFile = file; pidFile += L".pid"; std::ofstream(pidFile) << pid;
    std::cout << pid << '\n' << "\x1b]2;Poppy lifetime ready\x07" << std::flush;
    Handle controller(OpenEventW(EVENT_MODIFY_STATE, FALSE, event.c_str())); if (controller) SetEvent(controller.value);
    if (mode == L"--exit-parent") { leaf.process.reset(); return 0; }
    Sleep(INFINITE); return 0;
}
struct Fixture {
    fs::path root = fs::temp_directory_path() / (L"Poppy lifetime " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Fixture() { check(fs::create_directory(root), "Cannot create lifetime fixture directory"); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};
void unlocked(const fs::path& file) {
    Handle lock(CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(static_cast<bool>(lock), "A descendant still owns its lock after shutdown returned");
}
void jobs() {
    Fixture fixture; ProcessJob first, second;
    check(first.create() && second.create(), "Cannot create lifetime jobs");
    const auto event = L"Local\\Poppy-lifetime-" + std::to_wstring(GetCurrentProcessId());
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, event.c_str()));
    Child sibling({executable().native(), L"--hold", (fixture.root / "sibling").native(), event}, &second);
    check(WaitForSingleObject(ready.value, 10000) == WAIT_OBJECT_0, "Sibling fixture did not start"); ResetEvent(ready.value);
    Child root({executable().native(), L"--tree", (fixture.root / "child").native(), event}, &first);
    check(WaitForSingleObject(ready.value, 10000) == WAIT_OBJECT_0, "Tree fixture did not start");
    DWORD pid = 0; std::ifstream(fixture.root / "child.pid") >> pid; Handle descendant(OpenProcess(SYNCHRONIZE, FALSE, pid));
    check(static_cast<bool>(descendant), "Cannot retain the descendant process");
    first.stop();
    check(WaitForSingleObject(root.process.value, 0) == WAIT_OBJECT_0 && WaitForSingleObject(descendant.value, 0) == WAIT_OBJECT_0,
          "Job shutdown returned before every process exited");
    unlocked(fixture.root / "child");
    check(WaitForSingleObject(sibling.process.value, 0) == WAIT_TIMEOUT, "Stopping one job also stopped a sibling");
    second.stop(); unlocked(fixture.root / "sibling");
}
void runners() {
    Fixture fixture;
    const auto event = L"Local\\Poppy-runner-" + std::to_wstring(GetCurrentProcessId());
    const auto completed = runProcess({executable().native(), L"--exit-parent", (fixture.root / "completed").native(), event}, fixture.root);
    check(completed.code == 0 && completed.error.empty(), "The parent fixture did not exit successfully");
    unlocked(fixture.root / "completed");
    const auto timeout = runProcess({executable().native(), L"--tree", (fixture.root / "timeout").native(), event}, fixture.root, 2000);
    check(timeout.error == L"Git operation timed out.", "The process runner did not time out");
    unlocked(fixture.root / "timeout");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4) return fixture(argv[1], argv[2], argv[3]);
        jobs(); runners(); std::cout << "Process lifetime tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
