#include "wsl_lifecycle.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace poppy::wsl {
namespace {
using Clock = std::chrono::steady_clock;
struct CommandResult { int code; std::string output; };
CommandResult docker(std::vector<std::string> arguments) {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC)) throw std::runtime_error("Cannot open Docker cleanup pipe");
    arguments.insert(arguments.begin(), "docker");
    std::vector<char*> argv; for (auto& argument : arguments) argv.push_back(argument.data()); argv.push_back(nullptr);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attributes, 0);
    pid_t child = -1;
    const auto error = posix_spawnp(&child, "docker", &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions); close(pipes[1]);
    if (error) { close(pipes[0]); return {error == ENOENT ? 127 : 126, {}}; }
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    CommandResult result{124, {}}; int status = 0; bool exited = false;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline) {
        char bytes[4096]; const auto count = read(pipes[0], bytes, sizeof(bytes));
        if (count > 0) {
            result.output.append(bytes, static_cast<size_t>(count));
            if (result.output.size() > 1024 * 1024) break;
        } else if (!count && exited) break;
        if (!exited) exited = waitpid(child, &status, WNOHANG) == child;
        pollfd fd{pipes[0], POLLIN, 0}; if (count <= 0) poll(&fd, 1, 10);
    }
    close(pipes[0]);
    if (!exited) { kill(-child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
    else result.code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return result;
}
std::vector<std::string> containerIds(const std::string& output) {
    std::istringstream stream(output); std::vector<std::string> ids; std::string id;
    while (stream >> id) {
        if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != id.npos || ids.size() >= 4096)
            throw std::runtime_error("Invalid Docker container list during terminal cleanup");
        ids.push_back(std::move(id));
    }
    return ids;
}
void stopContainers(const std::string& socket) {
    for (;;) {
        const auto listed = docker({"ps", "--all", "--quiet", "--no-trunc"});
        // Docker is optional for ordinary WSL shells.
        if (listed.code == 127) return;
        if (listed.code) throw std::runtime_error("Cannot query Docker for this terminal's containers");
        const auto candidates = containerIds(listed.output);
        if (candidates.empty()) return;
        const auto format = "{{range .Mounts}}{{if eq .Source \"" + socket + "\"}}{{$.Id}}{{end}}{{end}}";
        std::vector<std::string> inspect{"inspect", "--format", format};
        inspect.insert(inspect.end(), candidates.begin(), candidates.end());
        const auto inspected = docker(std::move(inspect));
        if (inspected.code) {
            // An auto-removed container can disappear between ps and inspect.
            // Rescan; never interpret a failed inspection as successful cleanup.
            poll(nullptr, 0, 100); continue;
        }
        const auto owned = containerIds(inspected.output);
        if (owned.empty()) return;
        for (const auto& id : owned) {
            if (std::find(candidates.begin(), candidates.end(), id) == candidates.end())
                throw std::runtime_error("Docker cleanup returned an unlisted container");
        }
        std::vector<std::string> stop{"stop", "--time", "1"}; stop.insert(stop.end(), owned.begin(), owned.end());
        // stop waits for the container to exit, then ps verifies it is gone.
        // --rm can remove it before stop returns, so recheck even on an error.
        docker(std::move(stop));
        std::vector<std::string> remove{"rm", "--force"}; remove.insert(remove.end(), owned.begin(), owned.end());
        docker(std::move(remove));
    }
}
struct Process {
    pid_t pid = -1, parent = -1;
    std::string start;
    char state = 0;
};
Process process(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat"); std::string line; std::getline(file, line);
    const auto end = line.rfind(')'); if (end == line.npos) return {};
    std::istringstream fields(line.substr(end + 1)); Process result; result.pid = pid;
    fields >> result.state >> result.parent;
    std::string unused; for (int field = 5; field < 22; ++field) fields >> unused;
    fields >> result.start; return fields ? result : Process{};
}
bool alive(const Process& expected) {
    const auto current = process(expected.pid);
    return current.start == expected.start && current.pid > 1 && current.state != 'Z' && current.state != 'X';
}
void signal(const Process& expected, int value) {
    // A PID can be reused while Docker is stopping. Pin the process before
    // checking its start time, and signal that identity rather than a new PID.
    const auto fd = static_cast<int>(syscall(SYS_pidfd_open, expected.pid, 0));
    if (fd >= 0) {
        const auto result = alive(expected) ? syscall(SYS_pidfd_send_signal, fd, value, nullptr, 0) : 0;
        const auto error = errno; close(fd);
        if (result && error != ESRCH) throw std::runtime_error("Cannot stop a terminal's Linux process");
    } else if (alive(expected) && kill(expected.pid, value) && errno != ESRCH)
        throw std::runtime_error("Cannot stop a terminal's Linux process");
}
void stopLinuxProcesses(const std::string& socket) {
    std::map<pid_t, Process> owned;
    const auto deadline = Clock::now() + std::chrono::milliseconds(700);
    for (;;) {
        std::map<pid_t, Process> all;
        auto directory = opendir("/proc"); if (!directory) throw std::runtime_error("Cannot inspect terminal processes");
        while (auto entry = readdir(directory)) {
            if (!*entry->d_name || std::strspn(entry->d_name, "0123456789") != std::strlen(entry->d_name)) continue;
            const auto pid = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
            const auto current = process(pid); if (current.pid > 1) all.emplace(pid, current);
        }
        closedir(directory);
        std::set<pid_t> protectedPids;
        for (auto pid = getpid(); pid > 1 && protectedPids.insert(pid).second;) {
            const auto found = all.find(pid); if (found == all.end()) break; pid = found->second.parent;
        }
        const auto key = "AICONTAINER_EVENT_SOCKET=" + socket;
        for (auto item = owned.begin(); item != owned.end();) {
            if (!alive(item->second)) item = owned.erase(item); else ++item;
        }
        for (const auto& item : all) {
            if (protectedPids.count(item.first)) continue;
            std::ifstream environment("/proc/" + std::to_string(item.first) + "/environ", std::ios::binary); std::string entry;
            while (std::getline(environment, entry, '\0')) if (entry == key) { owned[item.first] = item.second; break; }
        }
        // Include descendants that deliberately changed their environment.
        bool added;
        do {
            added = false;
            for (const auto& item : all) {
                if (protectedPids.count(item.first) || owned.count(item.first)) continue;
                const auto parent = owned.find(item.second.parent);
                const auto captured = all.find(item.second.parent);
                if (parent != owned.end() && captured != all.end() && captured->second.start == parent->second.start) {
                    owned.emplace(item); added = true;
                }
            }
        } while (added);
        std::vector<std::pair<size_t, Process>> running;
        for (const auto& item : owned) if (alive(item.second)) {
            size_t depth = 0; std::set<pid_t> parents;
            for (auto parent = item.second.parent; parents.insert(parent).second;) {
                const auto found = owned.find(parent); if (found == owned.end()) break;
                ++depth; parent = found->second.parent;
            }
            running.emplace_back(depth, item.second);
        }
        if (running.empty()) return;
        std::sort(running.begin(), running.end(), [](const auto& left, const auto& right) { return left.first > right.first; });
        for (const auto& item : running) signal(item.second, Clock::now() < deadline ? SIGTERM : SIGKILL);
        poll(nullptr, 0, 20);
    }
}
}
void cleanupTerminalSocket(const std::string& socket) {
    // Prevent new aicontainer launches using this socket. Existing bind mounts
    // remain identifiable by their original Source path in Docker inspect.
    unlink(socket.c_str());
    bool reported = false;
    for (;;) {
        try {
            stopContainers(socket);
            stopLinuxProcesses(socket);
            // Catch a docker run that was already being created at shutdown.
            stopContainers(socket);
            return;
        } catch (const std::exception& error) {
            if (!reported) { std::cerr << error.what() << "; waiting to finish terminal cleanup\n"; reported = true; }
            poll(nullptr, 0, 500);
        }
    }
}
}
