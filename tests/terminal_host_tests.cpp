#include "terminal_host.h"
#include "wsl_protocol.h"
#include <objbase.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>

using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string identity() {
    GUID guid{}; check(SUCCEEDED(CoCreateGuid(&guid)), "Cannot create fixture identity");
    return wsl::sha256(std::string_view(reinterpret_cast<const char*>(&guid), sizeof(guid))).substr(0, 32);
}
fs::path executable() { return applicationDirectory() / L"poppy_terminal_host_tests.exe"; }
void output(HANDLE handle, std::string_view bytes) {
    while (!bytes.empty()) { DWORD count = 0; check(WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count, "Cannot write fixture output"); bytes.remove_prefix(count); }
}
template<class F> void wait(F ready, const char* message) {
    const auto deadline = GetTickCount64() + 15000;
    while (!ready()) { check(GetTickCount64() < deadline, message); Sleep(10); }
}
Handle child(const std::vector<std::wstring>& args) {
    auto line = processCommandLine(args); STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
    check(CreateProcessW(args.front().c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE, "Cannot start controller fixture");
    Handle thread(info.hThread); return Handle(info.hProcess);
}
int lockFixture(const fs::path& root) {
    Handle lock(CreateFileW((root / L"descendant.lock").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
    check(static_cast<bool>(lock), "Cannot acquire descendant lock");
    std::ofstream(root / "descendant.pid") << GetCurrentProcessId();
    Sleep(INFINITE); return 0;
}
int shellFixture(const fs::path& root) {
    Handle lock(CreateFileW((root / L"conversation.lock").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
    check(static_cast<bool>(lock), "Cannot acquire shell lock");
    auto leaf = child({executable().native(), L"--lock", root.native()});
    wait([&] { return fs::exists(root / "descendant.pid"); }, "Descendant did not start");
    fs::create_directory(root / "work"); check(SetCurrentDirectoryW((root / "work").c_str()), "Cannot set shell directory");
    check(SetEnvironmentVariableW(L"POPPY_TERMINAL_STATE", L"kept"), "Cannot set shell environment");
    const auto pid = std::to_string(GetCurrentProcessId());
    std::ofstream(root / "shell.pid") << pid;
    DWORD mode = 0; const auto out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
    if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    std::mutex outputMutex;
    const auto emit = [&](const std::string& text) { std::lock_guard<std::mutex> guard(outputMutex); output(out, text); };
    for (int i = 0; i < 60; ++i) emit("scrollback " + std::to_string(i) + "\r\n");
    emit("\x1b[?1049h\x1b[?2004h\x1b[?1hAI screen\x1b]2;ready-" + pid + "\x07");
    std::atomic<bool> stopping{false};
    NativeThread background([&] {
        while (!stopping && !fs::exists(root / "during")) Sleep(10);
        if (!stopping) {
            // Enough output to require a bounded snapshot while detached.
            for (int i = 0; i < 1000; ++i) emit(u8"detached output 日本語😀\r\n");
            emit("\x1b]2;during-" + pid + "\x07"); std::ofstream(root / "during.done") << "done";
        }
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find("probe") != line.npos) {
            wchar_t value[32]{}; GetEnvironmentVariableW(L"POPPY_TERMINAL_STATE", value, 32);
            emit("\x1b[?1049l\r\n" + fs::current_path().u8string() + " " + utf8(value) + "\r\n\x1b]2;probe-" + pid + "\x07");
        }
        if (line.find("quit") != line.npos) break;
    }
    stopping = true; background.join(); return 0;
}
int relayFixture(const fs::path& root) {
    const auto out = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto send = [&](const wsl::Message& message) { output(out, wsl::encodeFrame({1, wsl::encodeMessage(message)})); };
    send({{"socket", "/tmp/poppy-events-terminal-host-test"}});
    std::atomic<bool> stopping{false};
    NativeThread events([&] {
        Sleep(30);
        send({{"type", "status"}, {"body", "{\"status\":\"running\",\"activityText\":\"before restart\"}"}});
        while (!stopping && !fs::exists(root / "complete")) Sleep(10);
        if (!stopping) { send({{"type", "status"}, {"body", "{\"status\":\"completed\",\"activityText\":\"during restart\"}"}}); std::ofstream(root / "complete.done") << "done"; }
    });
    char buffer[4096]; DWORD count = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) && count) { }
    stopping = true; events.join(); std::ofstream(root / "relay.stopped") << "done"; return 0;
}
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("Poppy terminal host " + identity());
    Fixture() { fs::create_directory(root); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    TerminalHostLaunch launch(bool relay = true) const {
        TerminalHostLaunch launch; launch.directory = root; launch.columns = 80; launch.rows = 10;
        launch.command = {executable().native(), L"--shell", root.native()};
        if (relay) { launch.eventCommand = {executable().native(), L"--relay", root.native()}; launch.requireEvents = true; }
        return launch;
    }
};
TerminalHostUpdate untilTitle(TerminalHostClient& client, TerminalModel& model, const std::string& title) {
    TerminalHostUpdate latest;
    wait([&] {
        latest = client.poll(); if (latest.snapshot) model = TerminalModel::fromSnapshot(latest.data); else model.feed(latest.data);
        return model.title() == title;
    }, "Terminal did not reach the expected state");
    return latest;
}
std::string pid(const fs::path& file) { std::string result; std::ifstream(file) >> result; return result; }
bool unlocked(const fs::path& file) { Handle handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)); return static_cast<bool>(handle); }
int controller(const std::string& session, const fs::path& root, bool crash) {
    TerminalHostLaunch launch; launch.directory = root; launch.columns = 80; launch.rows = 10;
    launch.command = {executable().native(), L"--shell", root.native()};
    launch.eventCommand = {executable().native(), L"--relay", root.native()}; launch.requireEvents = true;
    TerminalHostClient client; auto initial = client.start(session, launch); auto model = TerminalModel::fromSnapshot(initial.data);
    wait([&] { return fs::exists(root / "shell.pid"); }, "Shell did not write its identity");
    untilTitle(client, model, "ready-" + pid(root / "shell.pid"));
    if (crash) { TerminateProcess(GetCurrentProcess(), 0); ExitProcess(0); }
    client.close(true, 3); return 0;
}
void restartAcrossProcesses() {
    Fixture fixture; const auto session = identity();
    auto oldUi = child({executable().native(), L"--controller", wide(session), fixture.root.native()});
    check(WaitForSingleObject(oldUi.value, 20000) == WAIT_OBJECT_0, "Old UI did not finish detaching");
    DWORD code = 1; GetExitCodeProcess(oldUi.value, &code); check(code == 0, "Old UI failed before restart");
    const auto shellPid = pid(fixture.root / "shell.pid"), leafPid = pid(fixture.root / "descendant.pid");
    Handle shell(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoul(shellPid))));
    Handle leaf(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoul(leafPid))));
    check(shell && leaf && !unlocked(fixture.root / "conversation.lock"), "Restart stopped the terminal or its descendants");
    std::ofstream(fixture.root / "during") << "run"; std::ofstream(fixture.root / "complete") << "run";
    wait([&] { return fs::exists(fixture.root / "during.done") && fs::exists(fixture.root / "complete.done"); }, "Detached output or AI status did not continue");
    TerminalHostClient nextUi; TerminalHostUpdate initial;
    check(nextUi.attach(session, initial), "New UI could not reconnect");
    auto model = TerminalModel::fromSnapshot(initial.data);
    check(initial.running && initial.relay && model.alternate() && model.bracketedPaste() && model.applicationCursor(), "Reattachment lost the live AI screen or input modes");
    std::string status = initial.status;
    wait([&] {
        if (status.find("completed") != status.npos && status.find("during restart") != status.npos) return true;
        const auto update = nextUi.poll(); if (update.snapshot) model = TerminalModel::fromSnapshot(update.data); else model.feed(update.data);
        if (!update.status.empty()) status = update.status;
        return false;
    }, "AI state was lost while detached");
    untilTitle(nextUi, model, "during-" + shellPid);
    nextUi.input("probe\r"); untilTitle(nextUi, model, "probe-" + shellPid);
    check(model.historySize() > 0, "Normal-screen scrollback was lost");
    std::string text;
    for (int row = 0; row < model.rows(); ++row) for (const auto& cell : model.row(row)) if (!cell.continuation) appendUtf8(text, cell.ch);
    check(text.find("kept") != text.npos && text.find("work") != text.npos, "Shell environment or current directory was reset");
    nextUi.resize(97, 13);
    wait([&] { auto update = nextUi.poll(); if (update.snapshot) model = TerminalModel::fromSnapshot(update.data); else model.feed(update.data); return model.columns() == 97 && model.rows() == 13; }, "Reconnected terminal did not resize");
    nextUi.close();
    check(WaitForSingleObject(shell.value, 0) == WAIT_OBJECT_0 && WaitForSingleObject(leaf.value, 0) == WAIT_OBJECT_0, "Normal close returned before descendants exited");
    check(unlocked(fixture.root / "conversation.lock") && unlocked(fixture.root / "descendant.lock") && fs::exists(fixture.root / "relay.stopped"), "Normal close did not finish process and relay cleanup");
    TerminalHostClient absent; check(!absent.attach(session, initial), "Normal close left a reconnectable host");
}
void crashedOwner() {
    Fixture fixture; const auto session = identity();
    auto owner = child({executable().native(), L"--crash", wide(session), fixture.root.native()});
    check(WaitForSingleObject(owner.value, 20000) == WAIT_OBJECT_0, "Crash fixture did not exit");
    check(fs::exists(fixture.root / "shell.pid"), "Crash fixture did not start its shell");
    wait([&] { return unlocked(fixture.root / "conversation.lock") && unlocked(fixture.root / "descendant.lock") && fs::exists(fixture.root / "relay.stopped"); }, "Unexpected owner exit abandoned terminal processes or its relay");
}
void failedLaunch() {
    Fixture fixture; TerminalHostClient client; const auto session = identity(); auto launch = fixture.launch(false);
    launch.command = {L"C:\\Poppy-terminal-command-does-not-exist.exe"};
    bool rejected = false;
    try { client.start(session, launch); } catch (const std::exception&) { rejected = true; }
    check(rejected, "A missing shell was reported as a successful launch");
    try { client.close(); } catch (const std::exception&) { }
    auto initial = client.start(session, fixture.launch(false)); check(initial.snapshot, "Failed startup prevented retry"); client.close();
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--shell") return shellFixture(argv[2]);
        if (argc == 3 && std::wstring(argv[1]) == L"--lock") return lockFixture(argv[2]);
        if (argc == 3 && std::wstring(argv[1]) == L"--relay") return relayFixture(argv[2]);
        if (argc == 4) return controller(utf8(argv[2]), argv[3], std::wstring(argv[1]) == L"--crash");
        restartAcrossProcesses(); crashedOwner(); failedLaunch(); std::cout << "Terminal host tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
