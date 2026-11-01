#include "win.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void inJob(HANDLE process, HANDLE job, const char* message) {
    BOOL member = FALSE;
    check(IsProcessInJob(process, job, &member) && member, message);
}
struct Child {
    poppy::Handle job, process;
    explicit Child(const std::wstring& executable) {
        job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        check(job && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
              "Cannot create a per-child job");
        auto command = poppy::processCommandLine({executable, L"--wait"});
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
              CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info) != FALSE,
              "Cannot create a child process");
        process.reset(info.hProcess); poppy::Handle thread(info.hThread);
        if (!AssignProcessToJobObject(job.value, process.value)) {
            TerminateProcess(process.value, 1);
            throw std::runtime_error("Cannot nest a per-child job inside the application group");
        }
        check(ResumeThread(thread.value) != static_cast<DWORD>(-1), "Cannot resume a child process");
    }
    ~Child() {
        job.reset();
        if (process) WaitForSingleObject(process.value, 5000);
    }
    void running(const char* message) const {
        check(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT, message);
    }
    void stop() {
        job.reset();
        check(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0,
              "Closing a per-child job did not stop its process");
    }
};
void processGroup() {
    // A debugger or terminal may already have put Poppy in a job. Exercise
    // inheritance and cancellation with such a launcher as well.
    poppy::Handle launcher(CreateJobObjectW(nullptr, nullptr));
    check(launcher && AssignProcessToJobObject(launcher.value, GetCurrentProcess()),
          "Cannot simulate a launcher job");
    auto group = poppy::createApplicationProcessGroup();
    check(static_cast<bool>(group), "Cannot create the application group");
    inJob(GetCurrentProcess(), group.value, "The application is absent from its group");

    std::wstring executable(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    check(count && count < executable.size(), "Cannot locate the child executable");
    executable.resize(count);
    Child first(executable), second(executable);
    for (const auto child : {first.process.value, second.process.value}) {
        inJob(child, group.value, "A child did not inherit the application group");
        inJob(child, launcher.value, "A child lost the launcher's outer job");
    }
    inJob(first.process.value, first.job.value, "The first child lost its cancellation job");
    inJob(second.process.value, second.job.value, "The second child lost its cancellation job");
    const auto completed = poppy::runProcess({executable, L"--echo"}, poppy::fs::temp_directory_path());
    check(completed.code == 0 && completed.error.empty() && completed.output == "child output",
          "The process runner failed inside the application group");
    const auto timedOut = poppy::runProcess({executable, L"--wait"}, poppy::fs::temp_directory_path(), 50);
    check(timedOut.error == L"Git operation timed out.", "The process runner did not time out its nested job");
    second.running("Timing out a process also stopped a sibling");
    first.stop();
    second.running("Stopping one child also stopped a sibling");
    inJob(GetCurrentProcess(), group.value, "Stopping one child affected the application group");

    // Closing the group must not terminate Poppy during normal shutdown, or
    // interfere with the kill-on-close behavior of its children's nested jobs.
    group.reset();
    second.running("Closing the application group prematurely stopped a child");
    second.stop();
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--wait") { Sleep(INFINITE); return 0; }
    if (argc == 2 && std::string(argv[1]) == "--echo") { std::cout << "child output"; return 0; }
    try {
        processGroup();
        std::cout << "process group tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
