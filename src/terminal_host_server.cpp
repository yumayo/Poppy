#include "terminal_host.h"
#include "terminal_profile.h"
#include "hook_pipe.h"
#include "ai_status.h"
#include <shellapi.h>
#include <sddl.h>
#include <algorithm>
#include <map>
#include <memory>

namespace poppy {
namespace {
using namespace terminalWire;
class Engine {
public:
    ~Engine() { try { stop(); } catch (...) { } }
    void start(TerminalHostLaunch launch) {
        launch_ = std::move(launch); model_ = TerminalModel(launch_.columns, launch_.rows);
        model_.reply = [this](std::string text) { input(std::move(text)); };
        model_.notificationError = [this](std::string code) { diagnostic_ = std::move(code); };
        if (!launch_.eventCommand.empty()) {
            try {
                const auto socket = events_.start(launch_.eventCommand, [this](const wsl::Message& message) {
                    const auto kind = wsl::field(message, "type");
                    if (message.size() != 2 || (kind != "status" && kind != "activity")) return;
                    const auto event = parseAiEvent(std::string(32, '0'), kind == "activity", wsl::field(message, "body"));
                    if (event) { std::lock_guard<std::mutex> lock(modelMutex_); if (ai_.apply(*event, GetTickCount64(), false)) aiChanged_ = true; }
                }, std::move(launch_.eventInput));
                launch_.environment.emplace_back(L"AICONTAINER_EVENT_SOCKET", wide(socket));
            } catch (const std::exception& error) {
                if (launch_.requireEvents) throw;
                relayError_ = error.what(); launch_.environment.emplace_back(L"AICONTAINER_EVENT_SOCKET", L"");
            }
        }
        startProcess(); started_ = true;
    }
    bool running() const { return !stopping_ && process_ && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT; }
    bool finished() const { return started_ && !stopped_ && !running(); }
    void input(std::string text) {
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (text.size() > 1024 * 1024 - writeBytes_) throw std::runtime_error("Terminal input queue is full");
        writeBytes_ += text.size(); writes_.push_back(std::move(text)); condition_.notify_all();
    }
    void resize(int columns, int rows) {
        { std::lock_guard<std::mutex> lock(writeMutex_); columns_ = columns; rows_ = rows; resizePending_ = true; }
        condition_.notify_all();
    }
    void detach(size_t scroll) { std::lock_guard<std::mutex> lock(modelMutex_); scroll_ = scroll; snapshotPending_ = true; pending_.clear(); }
    TerminalHostUpdate update(bool force = false) {
        std::lock_guard<std::mutex> lock(modelMutex_);
        TerminalHostUpdate result; result.running = running(); result.relay = events_.running(); result.snapshot = force || snapshotPending_;
        if (result.snapshot) { result.data = model_.snapshot(); result.scroll = std::min(scroll_, model_.historySize()); }
        else result.data = std::move(pending_);
        pending_.clear(); snapshotPending_ = false;
        if (force || !relayErrorSent_) result.relayError = relayError_;
        relayErrorSent_ = true; result.diagnostic = std::exchange(diagnostic_, {});
        if ((force || aiChanged_) && ai_.hookObserved) {
            const char* status = ai_.state == AiState::Running ? "running" : ai_.state == AiState::Waiting ? "waiting" : ai_.state == AiState::Paused ? "interrupted" : ai_.state == AiState::Completed ? "completed" : "none";
            result.status = "{\"status\":" + jsonString(status) + ",\"activityText\":" + jsonString(ai_.activity) + "}";
        }
        aiChanged_ = false; return result;
    }
    void stop() {
        if (stopped_) return;
        stopped_ = true;
        const bool wasRunning = running();
        // Keep ConPTY draining while the relay cleans Docker and Linux children.
        const bool cleaned = events_.stop();
        bool confirmed = true;
        if (!cleaned && process_ && !launch_.stopCommand.empty()) {
            const auto result = runProcess(launch_.stopCommand, launch_.directory, INFINITE);
            confirmed = !wasRunning || result.code == 0;
        }
        stopping_ = true; condition_.notify_all(); job_.stop(0);
        if (writer_.joinable()) {
            while (WaitForSingleObject(writer_.native_handle(), 0) == WAIT_TIMEOUT) { CancelSynchronousIo(writer_.native_handle()); WaitForSingleObject(writer_.native_handle(), 10); }
            writer_.join();
        }
        if (console_) { ClosePseudoConsole(console_); console_ = nullptr; }
        if (reader_.joinable()) { CancelSynchronousIo(reader_.native_handle()); reader_.join(); }
        input_.reset(); output_.reset(); process_.reset();
        if (!confirmed) throw std::runtime_error("Could not confirm that WSL terminal child processes exited. Check the processes in this distribution.");
    }
private:
    void startProcess() {
        const auto fail = [] { throw std::runtime_error(utf8(winError())); };
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, nullptr, 0)) fail();
        Handle childInput(read); input_.reset(write);
        if (!CreatePipe(&read, &write, nullptr, 0)) fail();
        output_.reset(read); Handle childOutput(write);
        const HRESULT hr = CreatePseudoConsole({static_cast<SHORT>(launch_.columns), static_cast<SHORT>(launch_.rows)}, childInput.value, childOutput.value, 0, &console_);
        if (FAILED(hr)) throw std::runtime_error("ConPTY could not start (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")");
        childInput.reset(); childOutput.reset();
        reader_.start([this] {
            char buffer[8192]; DWORD count = 0; bool parserFailed = false;
            while (ReadFile(output_.value, buffer, sizeof(buffer), &count, nullptr) && count) {
                if (parserFailed) continue; // ClosePseudoConsole still needs its output drained.
                std::lock_guard<std::mutex> lock(modelMutex_);
                try { model_.feed(std::string_view(buffer, count)); }
                catch (const std::exception&) { parserFailed = true; stopping_ = true; condition_.notify_all(); continue; }
                if (!snapshotPending_) {
                    if (pending_.size() + count <= 256 * 1024) pending_.append(buffer, count);
                    else { pending_.clear(); snapshotPending_ = true; }
                }
            }
        });
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size); auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) fail();
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Delete() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console_, sizeof(console_), nullptr, nullptr)) fail();
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
        const auto args = launch_.command.empty() ? terminalShellCommand(TerminalShell::PowerShell, system) : launch_.command;
        auto command = processCommandLine(args);
        if (!job_.create()) fail();
        struct Less { bool operator()(const std::wstring& a, const std::wstring& b) const { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; } };
        std::map<std::wstring, std::wstring, Less> inherited;
        if (auto block = GetEnvironmentStringsW()) {
            for (auto entry = block; *entry; entry += wcslen(entry) + 1) {
                const std::wstring value(entry); const auto split = value.find(L'=', value.front() == L'=' ? 1 : 0);
                if (split != std::wstring::npos) inherited[value.substr(0, split)] = value.substr(split + 1);
            }
            FreeEnvironmentStringsW(block);
        }
        for (const auto& value : launch_.environment) inherited[value.first] = value.second;
        const auto previous = inherited[L"WSLENV"]; std::wstring wslenv;
        for (size_t p = 0; p < previous.size();) {
            const auto end = previous.find(L':', p); const auto token = previous.substr(p, end == previous.npos ? end : end - p);
            if (token.substr(0, token.find(L'/')) != L"AICONTAINER_EVENT_SOCKET" && !token.empty()) { if (!wslenv.empty()) wslenv += L':'; wslenv += token; }
            if (end == previous.npos) break;
            p = end + 1;
        }
        if (!inherited[L"AICONTAINER_EVENT_SOCKET"].empty()) {
            if (!wslenv.empty()) wslenv += L':';
            wslenv += L"AICONTAINER_EVENT_SOCKET/u";
            auto& excluded = inherited[L"MSYS2_ENV_CONV_EXCL"];
            if (excluded != L"*") { if (!excluded.empty() && excluded.back() != L';') excluded += L';'; excluded += L"AICONTAINER_EVENT_SOCKET"; }
        }
        inherited[L"WSLENV"] = std::move(wslenv);
        std::vector<wchar_t> environment;
        for (const auto& value : inherited) { const auto entry = value.first + L"=" + value.second; environment.insert(environment.end(), entry.begin(), entry.end()); environment.push_back(0); }
        environment.push_back(0);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(args.front().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                environment.data(), launch_.directory.c_str(), &startup.StartupInfo, &info)) throw std::runtime_error("Could not start " + utf8(args.front()) + ": " + utf8(winError()));
        process_.reset(info.hProcess); Handle thread(info.hThread);
        if (!AssignProcessToJobObject(job_.handle(), process_.value)) { TerminateProcess(process_.value, 1); WaitForSingleObject(process_.value, INFINITE); fail(); }
        writer_.start([this] {
            for (;;) {
                std::string text; bool changeSize = false; int columns = 0, rows = 0;
                {
                    std::unique_lock<std::mutex> lock(writeMutex_); condition_.wait(lock, [this] { return stopping_ || resizePending_ || !writes_.empty(); });
                    if (stopping_) return;
                    if (resizePending_) { changeSize = true; columns = columns_; rows = rows_; resizePending_ = false; }
                    if (!writes_.empty()) { text = std::move(writes_.front()); writes_.pop_front(); writeBytes_ -= text.size(); }
                }
                if (changeSize) {
                    { std::lock_guard<std::mutex> lock(modelMutex_); model_.resize(columns, rows); pending_.clear(); snapshotPending_ = true; }
                    ResizePseudoConsole(console_, {static_cast<SHORT>(columns), static_cast<SHORT>(rows)});
                }
                size_t offset = 0;
                while (offset < text.size() && !stopping_) {
                    DWORD count = 0;
                    if (!WriteFile(input_.value, text.data() + offset, static_cast<DWORD>(text.size() - offset), &count, nullptr) || !count) return;
                    offset += count;
                }
            }
        });
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) fail();
    }
    TerminalHostLaunch launch_;
    TerminalModel model_;
    AiStatus ai_;
    HookPipe events_;
    std::mutex modelMutex_, writeMutex_;
    std::condition_variable condition_;
    std::string pending_, relayError_, diagnostic_;
    std::deque<std::string> writes_;
    size_t writeBytes_ = 0, scroll_ = 0;
    bool snapshotPending_ = true, aiChanged_ = false, resizePending_ = false, started_ = false, stopped_ = false, relayErrorSent_ = false;
    int columns_ = 80, rows_ = 24;
    std::atomic<bool> stopping_{false};
    Handle input_, output_, process_;
    ProcessJob job_;
    HPCON console_ = nullptr;
    NativeThread reader_, writer_;
};
Handle createPipe(const std::string& session) {
    Handle token; HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw std::runtime_error("Cannot read terminal host user");
    token.reset(raw); DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    if (!GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) throw std::runtime_error("Cannot read terminal host SID");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) throw std::runtime_error("Cannot encode terminal host SID");
    const std::wstring dacl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("Cannot secure terminal pipe");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    Handle pipe(CreateNamedPipeW(pipeName(session).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security));
    LocalFree(descriptor);
    if (!pipe) throw std::runtime_error("Cannot create terminal session pipe: " + utf8(winError()));
    return pipe;
}
int serve(const std::string& session) {
    auto pipe = createPipe(session); Engine engine; bool initialized = false, quit = false;
    Handle detachedOwner;
    auto expires = GetTickCount64() + 15000;
    while (!quit && GetTickCount64() < expires) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); OVERLAPPED accept{}; accept.hEvent = event.value;
        const BOOL connected = ConnectNamedPipe(pipe.value, &accept); const auto error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error != ERROR_PIPE_CONNECTED && error != ERROR_IO_PENDING) break;
        if (error == ERROR_IO_PENDING) {
            while (WaitForSingleObject(event.value, 50) == WAIT_TIMEOUT && GetTickCount64() < expires) {
                if (detachedOwner && WaitForSingleObject(detachedOwner.value, 0) == WAIT_TIMEOUT) expires = GetTickCount64() + 120000;
                if (engine.finished()) engine.stop();
            }
            if (GetTickCount64() >= expires) { CancelIoEx(pipe.value, &accept); DWORD count = 0; GetOverlappedResult(pipe.value, &accept, &count, TRUE); break; }
            DWORD count = 0; if (!GetOverlappedResult(pipe.value, &accept, &count, FALSE)) break;
        }
        bool detached = false;
        try {
            ULONG pid = 0;
            if (!GetNamedPipeClientProcessId(pipe.value, &pid)) throw std::runtime_error("Cannot identify terminal owner");
            Handle owner(OpenProcess(SYNCHRONIZE, FALSE, pid)); if (!owner) throw std::runtime_error("Cannot monitor terminal owner");
            detachedOwner.reset();
            const auto cancelled = [&] { return WaitForSingleObject(owner.value, 0) != WAIT_TIMEOUT; };
            auto packet = readPacket(pipe.value, cancelled);
            if (packet.op == Op::Start && !initialized) { engine.start(decodeLaunch(packet.data)); initialized = true; }
            else if (packet.op == Op::Attach && initialized) { Reader versionReader(packet.data); if (versionReader.number() != version) throw std::runtime_error("Incompatible terminal host; close terminals before updating this version"); versionReader.finish(); }
            else throw std::runtime_error("Terminal session cannot accept this connection");
            writePacket(pipe.value, Op::Update, encodeUpdate(engine.update(true)));
            for (;;) {
                if (engine.finished()) engine.stop();
                packet = readPacket(pipe.value, cancelled); Reader in(packet.data);
                if (packet.op == Op::Poll) { in.finish(); writePacket(pipe.value, Op::Update, encodeUpdate(engine.update())); }
                else if (packet.op == Op::Input) { if (packet.data.size() > 1024 * 1024) in.invalid(); engine.input(std::move(packet.data)); }
                else if (packet.op == Op::Resize) {
                    const auto columns = static_cast<int>(in.bounded(2, 500)), rows = static_cast<int>(in.bounded(1, 200)); in.finish(); engine.resize(columns, rows);
                } else if (packet.op == Op::Detach) {
                    const auto scroll = in.bounded(0, 1000); in.finish(); engine.detach(scroll);
                    expires = GetTickCount64() + 120000; detachedOwner = std::move(owner); detached = true; writePacket(pipe.value, Op::Ack, {}); break;
                } else if (packet.op == Op::Stop) { in.finish(); writePacket(pipe.value, Op::Ack, {}); quit = true; break; }
                else throw std::runtime_error("Invalid terminal request");
            }
        } catch (const std::exception& failure) {
            // An unexpected disconnect or dead owner retains ordinary cleanup.
            try { writePacket(pipe.value, Op::Error, failure.what()); } catch (...) { }
            quit = true;
        }
        DisconnectNamedPipe(pipe.value);
        if (!detached) quit = true;
    }
    engine.stop(); return 0;
}
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) return 1;
        std::unique_ptr<void, decltype(&LocalFree)> memory(args, &LocalFree);
        if (count != 2) return 1;
        return poppy::serve(poppy::utf8(args[1]));
    } catch (...) { return 1; }
}
