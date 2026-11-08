#include "hook_pipe.h"
#include <algorithm>
#include <stdexcept>

namespace poppy {
std::string HookPipe::start(const std::vector<std::wstring>& command, std::function<void(const wsl::Message&)> receive, std::string input) {
    stop();
    try {
        if (command.empty()) throw std::runtime_error("WSL notifications are unavailable");
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &sa, 0)) throw std::runtime_error("Cannot open notification input pipe");
        Handle childInput(read); input_.reset(write);
        if (!CreatePipe(&read, &write, &sa, 0)) throw std::runtime_error("Cannot open notification output pipe");
        output_.reset(read); Handle childOutput(write);
        Handle diagnostics(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
        if (!diagnostics || !SetHandleInformation(input_.value, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(output_.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("Cannot configure notification pipes");
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size); auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) throw std::runtime_error("Cannot initialize notification process");
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Delete() { DeleteProcThreadAttributeList(value); } } cleanup{list};
        HANDLE inherited[]{childInput.value, childOutput.value, diagnostics.value};
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) throw std::runtime_error("Cannot inherit notification pipes");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childInput.value; startup.StartupInfo.hStdOutput = childOutput.value; startup.StartupInfo.hStdError = diagnostics.value; startup.lpAttributeList = list;
        if (!job_.create()) throw std::runtime_error("Cannot track notification process");
        auto arguments = processCommandLine(command); PROCESS_INFORMATION pi{};
        wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
        const fs::path directory(system);
        if (!CreateProcessW(command.front().c_str(), arguments.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &pi)) throw std::runtime_error("Cannot start WSL notifications");
        process_.reset(pi.hProcess); Handle thread(pi.hThread);
        if (!AssignProcessToJobObject(job_.handle(), process_.value)) { TerminateProcess(process_.value, 1); WaitForSingleObject(process_.value, INFINITE); process_.reset(); throw std::runtime_error("Cannot track WSL notifications"); }
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) { job_.stop(); throw std::runtime_error("Cannot resume WSL notifications"); }
        childInput.reset(); childOutput.reset();
        stopping_ = false;
        if (!input.empty()) uploader_.start([this, bytes = std::move(input)] {
            size_t offset = 0;
            while (offset < bytes.size() && !stopping_) {
                DWORD count = 0;
                if (!WriteFile(input_.value, bytes.data() + offset, static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, wsl::maxChunk)), &count, nullptr) || !count) return;
                offset += count;
            }
        });
        wsl::FrameReader frames; std::string socket;
        const auto deadline = GetTickCount64() + 5000;
        while (socket.empty()) {
            DWORD available = 0, count = 0; char bytes[4096];
            if (!PeekNamedPipe(output_.value, nullptr, 0, nullptr, &available, nullptr)) throw std::runtime_error("WSL notification relay exited");
            if (available) {
                if (!ReadFile(output_.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) || !count) throw std::runtime_error("WSL notification handshake failed");
                frames.feed(std::string_view(bytes, count), [&](wsl::Frame frame) {
                    if (frame.kind != 1) throw std::runtime_error("Invalid notification handshake");
                    const auto message = wsl::decodeMessage(frame.payload);
                    if (socket.empty()) {
                        socket = wsl::field(message, "socket");
                        if (message.size() != 1 || socket.rfind("/tmp/poppy-events-", 0) != 0 || socket.size() >= 108 || socket.find('\0') != socket.npos) throw std::runtime_error("Invalid notification socket");
                    } else receive(message);
                });
            } else {
                if (GetTickCount64() >= deadline || WaitForSingleObject(process_.value, 15) == WAIT_OBJECT_0) throw std::runtime_error("WSL notification relay did not start");
            }
        }
        stopping_ = false; running_ = true; ready_ = true;
        reader_.start([this, receive = std::move(receive), stream = std::move(frames)]() mutable {
            try {
                while (!stopping_) {
                    char bytes[4096]; DWORD count = 0, available = 0;
                    if (!PeekNamedPipe(output_.value, nullptr, 0, nullptr, &available, nullptr)) { stream.finish(); break; }
                    if (available) {
                        if (!ReadFile(output_.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) || !count) break;
                        stream.feed(std::string_view(bytes, count), [&](wsl::Frame frame) {
                            if (frame.kind != 1) throw std::runtime_error("Invalid notification frame");
                            receive(wsl::decodeMessage(frame.payload));
                        });
                    } else if (WaitForSingleObject(process_.value, 15) == WAIT_OBJECT_0) break;
                }
            } catch (...) { }
            running_ = false;
        });
        return socket;
    } catch (...) { stop(); throw; }
}
bool HookPipe::stop() {
    stopping_ = true;
    if (uploader_.joinable()) {
        while (WaitForSingleObject(uploader_.native_handle(), 0) == WAIT_TIMEOUT) {
            CancelSynchronousIo(uploader_.native_handle()); WaitForSingleObject(uploader_.native_handle(), 10);
        }
        uploader_.join();
    }
    input_.reset();
    reader_.join();
    // EOF asks the Linux relay to stop its containers and Linux processes.
    // Killing wsl.exe after a fixed timeout would abandon that cleanup.
    bool cleaned = false;
    if (process_) {
        WaitForSingleObject(process_.value, INFINITE);
        DWORD code = 1; cleaned = ready_ && GetExitCodeProcess(process_.value, &code) && code == 0;
    }
    job_.stop();
    process_.reset(); output_.reset(); running_ = false; ready_ = false;
    return cleaned;
}
}
