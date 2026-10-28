#include "hook_pipe.h"
#include "project_provider.h"
#include <algorithm>
#include <stdexcept>

namespace poppy {
std::string HookPipe::start(const std::vector<std::wstring>& command, std::function<void(const wsl::Message&)> receive) {
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
        job_.reset(CreateJobObjectW(nullptr, nullptr)); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_ || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) throw std::runtime_error("Cannot track notification process");
        auto arguments = processCommandLine(command); PROCESS_INFORMATION pi{};
        const auto directory = windowsProcessDirectory();
        if (!CreateProcessW(command.front().c_str(), arguments.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &pi)) throw std::runtime_error("Cannot start WSL notifications");
        process_.reset(pi.hProcess); Handle thread(pi.hThread);
        if (!AssignProcessToJobObject(job_.value, process_.value)) { TerminateProcess(process_.value, 1); throw std::runtime_error("Cannot track WSL notifications"); }
        ResumeThread(thread.value); childInput.reset(); childOutput.reset();
        wsl::FrameReader frames; std::string socket;
        const auto deadline = GetTickCount64() + 5000;
        while (socket.empty()) {
            DWORD available = 0, count = 0; char bytes[4096];
            if (!PeekNamedPipe(output_.value, nullptr, 0, nullptr, &available, nullptr)) throw std::runtime_error("WSL notification relay exited");
            if (available) {
                if (!ReadFile(output_.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) || !count) throw std::runtime_error("WSL notification handshake failed");
                frames.feed(std::string_view(bytes, count), [&](wsl::Frame frame) {
                    if (frame.kind != 1 || !socket.empty()) throw std::runtime_error("Invalid notification handshake");
                    const auto message = wsl::decodeMessage(frame.payload); socket = wsl::field(message, "socket");
                    if (message.size() != 1 || socket.rfind("/tmp/poppy-events-", 0) != 0 || socket.size() >= 108 || socket.find('\0') != socket.npos) throw std::runtime_error("Invalid notification socket");
                });
            } else {
                if (GetTickCount64() >= deadline || WaitForSingleObject(process_.value, 15) == WAIT_OBJECT_0) throw std::runtime_error("WSL notification relay did not start");
            }
        }
        stopping_ = false; running_ = true;
        reader_.start([this, receive = std::move(receive)] {
            try {
                wsl::FrameReader stream;
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
void HookPipe::stop() {
    stopping_ = true; input_.reset();
    reader_.join();
    if (process_ && WaitForSingleObject(process_.value, 2000) != WAIT_OBJECT_0 && job_) TerminateJobObject(job_.value, 1);
    process_.reset(); output_.reset(); job_.reset(); running_ = false;
}
}
