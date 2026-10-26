#include "wsl_protocol.h"
#include "resource.h"
#include "text.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <limits.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <termios.h>

namespace poppy::wsl {
namespace {
using Clock = std::chrono::steady_clock;
volatile sig_atomic_t terminalStop = 0;
void terminalSignal(int) { terminalStop = 1; }
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
sockaddr_un terminalAddress(const std::string& token, socklen_t& length) {
    if (token.empty() || token.size() > 64 || token.find_first_not_of("0123456789abcdef-") != token.npos) throw std::runtime_error("Invalid terminal token");
    const auto name = "poppy-terminal-" + std::to_string(getuid()) + '-' + token;
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (name.size() + 1 >= sizeof(address.sun_path)) throw std::runtime_error("Terminal token is too long");
    memcpy(address.sun_path + 1, name.data(), name.size()); length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size()); return address;
}
bool stopTerminal(const std::string& token) {
    socklen_t length; const auto address = terminalAddress(token, length); Fd socketFd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (socketFd.value < 0 || connect(socketFd.value, reinterpret_cast<const sockaddr*>(&address), length)) return false;
    const char command = 'S'; if (send(socketFd.value, &command, 1, MSG_NOSIGNAL) != 1) return false;
    pollfd fd{socketFd.value, POLLIN, 0}; if (poll(&fd, 1, 3000) <= 0) return false;
    char result = 0; return recv(socketFd.value, &result, 1, 0) == 1 && result == 'K';
}
std::vector<pid_t> descendants(pid_t parent) {
    std::vector<pid_t> result; std::deque<pid_t> pending{parent};
    while (!pending.empty() && result.size() < 16384) {
        const auto pid = pending.front(); pending.pop_front();
        std::ifstream children("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children"); pid_t child;
        while (children >> child) if (child > 1) { result.push_back(child); pending.push_back(child); }
    }
    return result;
}
bool cleanupTerminal() {
    for (const auto signal : {SIGTERM, SIGKILL}) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(signal == SIGTERM ? 700 : 1000);
        do {
            const auto children = descendants(getpid());
            for (auto it = children.rbegin(); it != children.rend(); ++it) kill(*it, signal);
            int status = 0; while (waitpid(-1, &status, WNOHANG) > 0) {}
            if (descendants(getpid()).empty()) return true;
            poll(nullptr, 0, 20);
        } while (Clock::now() < deadline);
    }
    return false;
}
int terminal(int argc, char** argv) {
    if (argc < 5) throw std::runtime_error("Invalid terminal invocation");
    const std::string directory = argv[2]; if (directory.empty() || directory[0] != '/' || chdir(directory.c_str())) throw std::runtime_error("Cannot enter the WSL terminal directory");
    socklen_t length; const auto address = terminalAddress(argv[3], length); Fd listener(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (listener.value < 0 || bind(listener.value, reinterpret_cast<const sockaddr*>(&address), length) || listen(listener.value, 1)) throw std::runtime_error("Cannot create terminal lifecycle socket");
    if (prctl(PR_SET_CHILD_SUBREAPER, 1)) throw std::runtime_error("Cannot supervise Linux terminal descendants");
    setenv("TERM", "xterm-256color", 1); setenv("COLORTERM", "truecolor", 1);
    for (int sig : {SIGHUP, SIGTERM, SIGINT}) signal(sig, terminalSignal);
    signal(SIGTTOU, SIG_IGN); signal(SIGPIPE, SIG_IGN);
    const auto parent = getpid(); const auto child = fork(); if (child < 0) throw std::runtime_error("Cannot start Linux shell");
    if (!child) {
        prctl(PR_SET_PDEATHSIG, SIGHUP); if (getppid() != parent) _exit(126);
        for (int sig : {SIGHUP, SIGTERM, SIGINT, SIGTTOU, SIGPIPE}) signal(sig, SIG_DFL);
        setpgid(0, 0); tcsetpgrp(STDIN_FILENO, getpid());
        execvp(argv[4], argv + 4);
        constexpr char message[] = "Cannot start the Linux shell or AI CLI. Check that it is installed and executable in this WSL distribution.\r\n";
        ::write(STDERR_FILENO, message, sizeof(message)-1); _exit(errno == ENOENT ? 127 : 126);
    }
    setpgid(child, child); tcsetpgrp(STDIN_FILENO, child);
    int status = 0; Fd controller; bool finished = false;
    while (!terminalStop) {
        if (waitpid(child, &status, WNOHANG) == child) { finished = true; break; }
        pollfd fds[]{{listener.value, POLLIN, 0}, {STDIN_FILENO, 0, 0}}; poll(fds, 2, 50);
        if (fds[1].revents & (POLLHUP | POLLERR)) break;
        if (fds[0].revents & POLLIN) {
            const auto socketFd = accept4(listener.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (socketFd >= 0) {
                ucred credentials{}; socklen_t size = sizeof(credentials);
                if (getsockopt(socketFd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 && credentials.uid == getuid()) {
                    pollfd fd{socketFd, POLLIN, 0}; poll(&fd, 1, 100); char command = 0;
                    if (recv(socketFd, &command, 1, 0) == 1 && command == 'S') { controller.value = socketFd; break; }
                }
                close(socketFd);
            }
        }
    }
    const bool cleaned = cleanupTerminal();
    if (controller.value >= 0) { const char result = cleaned ? 'K' : 'E'; send(controller.value, &result, 1, MSG_NOSIGNAL); }
    if (!cleaned) { std::cerr << "Linux terminal descendant cleanup could not be confirmed\n"; return 1; }
    return finished && WIFEXITED(status) ? WEXITSTATUS(status) : 0;
}
[[noreturn]] void failure(const std::string& what) { throw std::runtime_error(what + ": " + std::strerror(errno)); }
std::pair<std::string,std::string> userInfo() {
    // WSL's local users are in /etc/passwd. Avoid NSS/dlopen so the shipped
    // static helper does not depend on the distribution's libc version.
    std::ifstream file("/etc/passwd"); std::string line;
    while (std::getline(file,line)) {
        std::vector<std::string> fields; size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i) if (i == line.size() || line[i] == ':') { fields.push_back(line.substr(start,i-start)); start = i + 1; }
        if (fields.size() != 7 || fields[2] != std::to_string(getuid())) continue;
        if (fields[5].empty() || fields[5][0] != '/' || !validUtf8(fields[5])) throw std::runtime_error("Unsupported Linux home directory");
        return {fields[5], !fields[6].empty() && fields[6][0] == '/' && validUtf8(fields[6]) ? fields[6] : "/bin/sh"};
    }
    throw std::runtime_error("Cannot find this WSL user in /etc/passwd");
}
std::string home() { return userInfo().first; }
std::string shell() { return userInfo().second; }
std::string path(const Message& m) { auto p = field(m, "path"); if (p == "~") p = home(); else if (p.rfind("~/", 0) == 0) p = home() + p.substr(1); if (p.empty() || p[0] != '/' || p.find('\0') != p.npos || !validUtf8(p)) throw std::runtime_error("Expected an absolute UTF-8 Linux path"); return p; }
std::string real(const std::string& p) { char* value = realpath(p.c_str(), nullptr); if (!value) failure("Cannot resolve path"); std::string result(value); free(value); if (!validUtf8(result)) throw std::runtime_error("The resolved path is not UTF-8"); return result; }
std::string stamp(const struct stat& s) {
    return std::to_string(s.st_dev) + ':' + std::to_string(s.st_ino) + ':' + std::to_string(s.st_size) + ':' + std::to_string(s.st_mtim.tv_sec) + ':' + std::to_string(s.st_mtim.tv_nsec) + ':' + std::to_string(s.st_ctim.tv_sec) + ':' + std::to_string(s.st_ctim.tv_nsec) + ':' + std::to_string(s.st_mode) + ':' + std::to_string(s.st_nlink) + ':' + std::to_string(s.st_uid) + ':' + std::to_string(s.st_gid);
}
struct Snapshot { std::string bytes, version, target; struct stat info{}; };
Snapshot read(const std::string& p, const std::atomic<bool>* cancelled = nullptr) {
    Snapshot result; struct stat link{}; if (lstat(p.c_str(), &link)) failure("Cannot inspect file");
    result.target = real(p);
    struct stat target{}; if (stat(result.target.c_str(), &target)) failure("Cannot inspect target");
    if (!S_ISREG(target.st_mode)) throw std::runtime_error("Only regular files can be opened");
    Fd fd(open(result.target.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)); if (fd.value < 0) failure("Cannot open file");
    if (fstat(fd.value, &result.info)) failure("Cannot inspect open file");
    if (!S_ISREG(result.info.st_mode)) throw std::runtime_error("Only regular files can be opened");
    if (result.info.st_size < 0 || static_cast<uint64_t>(result.info.st_size) > maxFileBytes) throw std::runtime_error("File exceeds the 16 MiB limit");
    char buffer[32768];
    for (;;) {
        if (cancelled && *cancelled) throw std::runtime_error("Request cancelled");
        const auto count = ::read(fd.value, buffer, sizeof(buffer)); if (count < 0) { if (errno == EINTR) continue; failure("Cannot read file"); } if (!count) break;
        if (result.bytes.size() + count > maxFileBytes) throw std::runtime_error("File exceeds the 16 MiB limit");
        result.bytes.append(buffer, static_cast<size_t>(count));
    }
    struct stat after{}, afterLink{}, targetNow{};
    if (fstat(fd.value, &after) || lstat(p.c_str(), &afterLink) || stat(result.target.c_str(), &targetNow)) failure("Cannot revalidate file");
    if (stamp(after) != stamp(result.info) || stamp(link) != stamp(afterLink) || stamp(targetNow) != stamp(result.info) || real(p) != result.target) throw std::runtime_error("File changed while it was being read; retry");
    result.version = sha256(stamp(result.info) + ':' + stamp(link) + ':' + result.target + ':' + sha256(result.bytes));
    return result;
}
struct Request { Message message; std::string bytes; std::atomic<bool> cancelled{false}; size_t size = 0; bool uploaded = false; };
class Server {
public:
    ~Server();
    void run();
private:
    void output(Frame frame);
    void send(const Message& m) { output({1, encodeMessage(m)}); }
    void reply(const Request& request, Message m);
    void body(const Request& request, const std::string& bytes);
    void accept(Frame frame);
    void execute(const std::shared_ptr<Request>& request);
    void worker();
    void watch(const std::string& p);
    void events();
    void save(Request& request);
    void git(Request& request);
    std::atomic<bool> stopping_{false};
    std::mutex mutex_, outputMutex_, watchMutex_;
    std::condition_variable condition_;
    std::map<uint64_t, std::shared_ptr<Request>> requests_;
    std::set<pid_t> gitChildren_;
    std::deque<std::shared_ptr<Request>> queue_;
    std::shared_ptr<Request> upload_;
    std::vector<std::thread> workers_;
    std::thread watcher_;
    Fd notify_{inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
    std::set<std::string> watched_;
    std::map<int,std::string> watchPaths_;
    bool watchLimited_ = false, drvfs_ = false;
    bool hello_ = false;
    size_t uploadBytes_ = 0;
};
Server::~Server() {
    stopping_ = true;
    { std::lock_guard<std::mutex> lock(mutex_); for (auto& item : requests_) item.second->cancelled = true; queue_.clear(); }
    condition_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    if (watcher_.joinable()) watcher_.join();
}
void Server::output(Frame frame) {
    const auto bytes = encodeFrame(frame); std::lock_guard<std::mutex> lock(outputMutex_); size_t offset = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    auto broken = [&] { std::lock_guard<std::mutex> lock(mutex_); for (const auto pid : gitChildren_) kill(-pid, SIGKILL); _exit(2); };
    while (offset < bytes.size()) {
        pollfd fd{STDOUT_FILENO, POLLOUT, 0}; if (poll(&fd, 1, 100) < 0 && errno != EINTR) broken();
        if (Clock::now() > deadline || stopping_) broken();
        if (fd.revents & (POLLHUP | POLLERR)) broken();
        if (!(fd.revents & POLLOUT)) continue;
        const auto count = ::write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) { if (errno == EINTR || errno == EAGAIN) continue; broken(); } offset += static_cast<size_t>(count);
    }
}
void Server::reply(const Request& request, Message message) {
    for (const auto& key : {"id", "projectId", "generation"}) message[key] = field(request.message, key);
    send(message);
}
void Server::body(const Request& request, const std::string& bytes) {
    size_t offset = 0;
    do { if (request.cancelled) throw std::runtime_error("Request cancelled"); const size_t count = std::min(maxChunk, bytes.size() - offset); output(dataFrame(number(request.message, "id"), offset, offset + count == bytes.size(), std::string_view(bytes).substr(offset, count))); offset += count; } while (offset < bytes.size());
}
void Server::accept(Frame frame) {
    if (frame.kind == 2) {
        const auto chunk = decodeChunk(frame); std::lock_guard<std::mutex> lock(mutex_);
        if (!upload_ || number(upload_->message, "id") != chunk.id || upload_->bytes.size() != chunk.offset || chunk.bytes.size() > upload_->size - upload_->bytes.size()) throw std::runtime_error("Unexpected file upload chunk");
        upload_->bytes += chunk.bytes;
        if (chunk.last) { if (upload_->bytes.size() != upload_->size) throw std::runtime_error("Truncated file upload"); upload_->uploaded = true; queue_.push_back(upload_); upload_.reset(); condition_.notify_one(); }
        return;
    }
    auto m = decodeMessage(frame.payload); const auto op = field(m, "op");
    if (!hello_ && op != "hello") throw std::runtime_error("The first WSL request must be hello");
    if (op == "hello") { if (hello_ || number(m, "protocolVersion") != protocolVersion) throw std::runtime_error("WSL protocol version mismatch or duplicate hello"); hello_ = true; }
    if (op == "shutdown") { stopping_ = true; return; }
    const auto id = number(m, "id"); if (!id) throw std::runtime_error("Invalid request ID");
    std::lock_guard<std::mutex> lock(mutex_);
    if (op == "cancel") { const auto found = requests_.find(number(m, "target")); if (found != requests_.end()) found->second->cancelled = true; return; }
    if (field(m, "projectId").empty() || field(m, "projectId").size() > 128) throw std::runtime_error("Invalid project ID");
    number(m, "generation");
    if (requests_.count(id) || requests_.size() >= 128) throw std::runtime_error("WSL request queue is full or ID is reused");
    auto r = std::make_shared<Request>(); r->message = std::move(m); requests_[id] = r;
    if (op == "saveFile") {
        if (upload_) throw std::runtime_error("Concurrent upload is unsupported");
        r->size = number(r->message, "size"); if (r->size > maxFileBytes || uploadBytes_ + r->size > 64 * 1024 * 1024) throw std::runtime_error("Save exceeds the file or upload queue limit"); uploadBytes_ += r->size; upload_ = r;
    } else { queue_.push_back(r); condition_.notify_one(); }
}
void Server::watch(const std::string& p) {
    std::lock_guard<std::mutex> lock(watchMutex_);
    if (watched_.count(p)) return;
    if (p.rfind("/mnt/", 0) == 0) drvfs_ = true;
    if (watchPaths_.size() >= 8192 || notify_.value < 0) { watchLimited_ = true; return; }
    const auto wd = inotify_add_watch(notify_.value, p.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW);
    if (wd < 0) { watchLimited_ = true; return; }
    const auto existing = watchPaths_.find(wd); if (existing != watchPaths_.end()) watched_.erase(existing->second);
    watchPaths_[wd] = p; watched_.insert(p);
}
void Server::events() {
    auto lastFallback = Clock::now(); alignas(inotify_event) char bytes[65536];
    while (!stopping_) {
        pollfd fd{notify_.value, POLLIN, 0}; poll(&fd, 1, 100); bool changed = false;
        if (fd.revents & POLLIN) { for (;;) { const auto count = ::read(notify_.value, bytes, sizeof(bytes)); if (count <= 0) break; changed = true; for (size_t i = 0; i + sizeof(inotify_event) <= static_cast<size_t>(count);) { const auto event = reinterpret_cast<const inotify_event*>(bytes + i); if (event->mask & IN_IGNORED) { std::lock_guard<std::mutex> lock(watchMutex_); watchLimited_ = true; const auto found = watchPaths_.find(event->wd); if (found != watchPaths_.end()) { watched_.erase(found->second); watchPaths_.erase(found); } } i += sizeof(inotify_event) + event->len; } } }
        bool fallback = false; { std::lock_guard<std::mutex> lock(watchMutex_); fallback = watchLimited_ || drvfs_; }
        if (fallback && Clock::now() - lastFallback >= std::chrono::seconds(2)) { changed = true; lastFallback = Clock::now(); }
        if (changed && !stopping_) send({{"event", "rescanRequired"}});
    }
}
void Server::save(Request& request) {
    const auto p = path(request.message); auto original = read(p, &request.cancelled);
    if (original.version != field(request.message, "version")) throw std::runtime_error("The file changed outside Poppy. Your edits have been kept. Reconcile the external changes before saving.");
    const auto& info = original.info;
    if (info.st_nlink != 1) throw std::runtime_error("Saving a file with multiple hard links is unsupported; your edits have been kept");
    if (info.st_uid != getuid()) throw std::runtime_error("Saving a file owned by another user is unsupported");
    if (info.st_mode & (S_ISUID | S_ISGID)) throw std::runtime_error("Saving a setuid/setgid file is unsupported");
    Fd source(open(original.target.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)); if (source.value < 0) failure("Cannot inspect original metadata");
    const auto attributes = flistxattr(source.value, nullptr, 0);
    if (attributes < 0 && errno != ENOTSUP) failure("Cannot inspect extended attributes");
    if (attributes > 0) throw std::runtime_error("Saving files with ACLs or extended attributes is unsupported; your edits have been kept");
    if (access(original.target.c_str(), W_OK)) failure("File is not writable");
    const auto parent = std::filesystem::path(original.target).parent_path().string();
    std::string pattern = parent + "/.poppy-save-XXXXXX"; std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
    Fd temporary(mkostemp(name.data(), O_CLOEXEC)); if (temporary.value < 0) failure("Cannot create temporary file");
    struct Cleanup { const char* name; ~Cleanup() { unlink(name); } } cleanup{name.data()};
    if (fchown(temporary.value, info.st_uid, info.st_gid) || fchmod(temporary.value, info.st_mode & 07777)) failure("Cannot preserve file ownership and mode");
    // Some parent directories give new files a default ACL. Refuse to discard
    // or silently introduce metadata which this protocol cannot preserve.
    const auto newAttributes = flistxattr(temporary.value, nullptr, 0);
    if (newAttributes > 0 || (newAttributes < 0 && errno != ENOTSUP)) throw std::runtime_error("Cannot preserve extended attributes in this directory");
    size_t offset = 0;
    while (offset < request.bytes.size()) { if (request.cancelled) throw std::runtime_error("Request cancelled"); const auto count = ::write(temporary.value, request.bytes.data() + offset, request.bytes.size() - offset); if (count < 0) { if (errno == EINTR) continue; failure("Cannot write temporary file"); } if (!count) throw std::runtime_error("Short write"); offset += count; }
    if (fsync(temporary.value)) failure("Cannot flush temporary file");
    if (read(p, &request.cancelled).version != original.version) throw std::runtime_error("The file changed before replacement. Your edits have been kept.");
    if (rename(name.data(), original.target.c_str())) failure("Cannot replace file");
    Fd directory(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.value < 0 || fsync(directory.value)) throw std::runtime_error("Replacement completed, but directory flush failed. Reload to verify before saving again.");
    Snapshot saved;
    try { saved = read(p); }
    catch (const std::exception&) { throw std::runtime_error("Replacement completed, but its new version could not be verified. Reload to verify before saving again."); }
    reply(request, {{"version", saved.version}, {"done", "1"}});
}
void Server::git(Request& request) {
    const auto p = path(request.message); const auto argc = number(request.message, "argc"); if (argc > 64) throw std::runtime_error("Too many Git arguments");
    std::vector<std::string> args{"git"}; for (size_t i = 0; i < argc; ++i) { auto arg = field(request.message, "arg" + std::to_string(i)); if (arg.find('\0') != arg.npos) throw std::runtime_error("Invalid Git argument"); args.push_back(std::move(arg)); }
    int pipes[2]; if (pipe2(pipes, O_CLOEXEC)) failure("Cannot create Git pipe"); Fd input(pipes[0]), outputFd(pipes[1]);
    std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
    std::unique_lock<std::mutex> childrenLock(mutex_);
    const pid_t parent = getpid(); const pid_t child = fork(); if (child < 0) failure("Cannot start Git");
    if (!child) {
        prctl(PR_SET_PDEATHSIG, SIGKILL); if (getppid() != parent) _exit(126); setpgid(0, 0);
        if (chdir(p.c_str()) || dup2(outputFd.value, 1) < 0 || dup2(outputFd.value, 2) < 0) _exit(126);
        const int nul = open("/dev/null", O_RDONLY); if (nul < 0 || dup2(nul, 0) < 0) _exit(126);
        execvp(argv[0], argv.data()); _exit(errno == ENOENT ? 127 : 126);
    }
    setpgid(child, child); gitChildren_.insert(child); childrenLock.unlock(); close(outputFd.value); outputFd.value = -1;
    fcntl(input.value, F_SETFL, O_NONBLOCK); int status = 0; bool exited = false, eof = false; std::string bytes;
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    auto terminate = [&] {
        kill(-child, SIGKILL);
        if (!exited) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        do { int descendant = 0; const auto reaped = waitpid(-child, &descendant, WNOHANG); if (reaped < 0 && errno == ECHILD) break; if (reaped > 0) continue; poll(nullptr, 0, 10); } while (Clock::now() < deadline);
        std::lock_guard<std::mutex> lock(mutex_); gitChildren_.erase(child);
    };
    try {
        while (!exited || !eof) {
            if (stopping_ || request.cancelled) throw std::runtime_error("Git operation cancelled");
            if (Clock::now() > deadline) throw std::runtime_error("Git operation timed out");
            pollfd fd{input.value, POLLIN, 0}; poll(&fd, 1, 50); char buffer[8192];
            for (;;) { const auto count = ::read(input.value, buffer, sizeof(buffer)); if (!count) { eof = true; break; } if (count < 0) { if (errno == EAGAIN || errno == EINTR) break; failure("Cannot read Git output"); } if (bytes.size() + count > 4 * 1024 * 1024) throw std::runtime_error("Git output exceeds the 4 MiB limit"); bytes.append(buffer, static_cast<size_t>(count)); }
            if (!exited) { const auto waited = waitpid(child, &status, WNOHANG); if (waited < 0) failure("Cannot wait for Git"); exited = waited == child; }
        }
    } catch (...) { terminate(); throw; }
    terminate();
    reply(request, {{"body", "1"}, {"size", std::to_string(bytes.size())}, {"exitCode", std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : 1)}}); body(request, bytes); reply(request, {{"done", "1"}});
}
void Server::execute(const std::shared_ptr<Request>& r) {
    try {
        if (r->cancelled) throw std::runtime_error("Request cancelled");
        const auto op = field(r->message, "op");
        if (op == "hello") {
            if (number(r->message, "protocolVersion") != protocolVersion) throw std::runtime_error("WSL protocol version mismatch");
            utsname system{}; if (uname(&system)) failure("Cannot detect architecture");
            reply(*r, {{"protocolVersion", std::to_string(protocolVersion)}, {"helperVersion", helperVersion}, {"uid", std::to_string(getuid())}, {"home", home()}, {"shell", shell()}, {"executable", real("/proc/self/exe")}, {"architecture", system.machine}, {"capabilities", "list,read,save,rename,index,git,inotify,terminal"}, {"done", "1"}});
        } else if (op == "connect") {
            const auto p = real(path(r->message)); struct stat info{}; if (stat(p.c_str(), &info)) failure("Cannot inspect project"); if (!S_ISDIR(info.st_mode)) throw std::runtime_error("The project path is not a directory"); watch(p); reply(*r, {{"path", p}, {"done", "1"}});
        } else if (op == "stat") {
            const auto p = path(r->message); struct stat info{}; if (lstat(p.c_str(), &info)) failure("Cannot inspect resource");
            reply(*r, {{"version", stamp(info)}, {"directory", S_ISDIR(info.st_mode) ? "1" : "0"}, {"link", S_ISLNK(info.st_mode) ? "1" : "0"}, {"done", "1"}});
        } else if (op == "readFile") {
            const auto p = path(r->message); const auto snapshot = read(p, &r->cancelled); watch(std::filesystem::path(snapshot.target).parent_path().string());
            reply(*r, {{"body", "1"}, {"size", std::to_string(snapshot.bytes.size())}, {"version", snapshot.version}}); body(*r, snapshot.bytes); reply(*r, {{"done", "1"}});
        } else if (op == "saveFile") save(*r);
        else if (op == "renameResource") {
            const auto p = path(r->message); const auto name = field(r->message, "name"); const auto parent = std::filesystem::path(p).parent_path().string();
            errno = 0; const auto limit = pathconf(parent.c_str(), _PC_NAME_MAX); if (limit < 0 && errno) failure("Cannot determine filename limit");
            if (!validPosixName(name, limit > 0 ? static_cast<size_t>(limit) : 255)) throw std::runtime_error("Invalid Linux filename");
            struct stat info{}; if (lstat(p.c_str(), &info)) failure("Cannot inspect rename source");
            if (stamp(info) != field(r->message, "version")) throw std::runtime_error("The rename source changed; retry");
            const auto destination = parent + '/' + name;
            if (syscall(SYS_renameat2, AT_FDCWD, p.c_str(), AT_FDCWD, destination.c_str(), 1 /* RENAME_NOREPLACE */)) failure("Cannot rename without overwriting");
            reply(*r, {{"path", destination}, {"done", "1"}});
        } else if (op == "listDirectory" || op == "indexFiles") {
            const auto root = path(r->message); std::deque<std::string> pending{root}; size_t entries = 0, directories = 0; bool truncated = false;
            while (!pending.empty()) {
                if (r->cancelled || stopping_) throw std::runtime_error("Request cancelled");
                const auto directory = std::move(pending.front()); pending.pop_front(); watch(directory);
                DIR* raw = opendir(directory.c_str()); if (!raw) { if (directory == root) failure("Cannot list directory"); continue; }
                std::unique_ptr<DIR, int(*)(DIR*)> dir(raw, closedir);
                while (auto entry = readdir(dir.get())) {
                    if (r->cancelled || stopping_) throw std::runtime_error("Request cancelled");
                    const std::string name = entry->d_name; if (name == "." || name == "..") continue;
                    const auto p = directory + (directory.back() == '/' ? "" : "/") + name;
                    if (name == ".git") {
                        auto gitDirectory = p; struct stat gitInfo{};
                        if (!lstat(p.c_str(), &gitInfo) && S_ISREG(gitInfo.st_mode)) { std::ifstream pointer(p); std::string line; std::getline(pointer,line); if (line.rfind("gitdir: ",0) == 0) { auto target = std::filesystem::path(line.substr(8)); gitDirectory = (target.is_absolute() ? target : std::filesystem::path(directory)/target).lexically_normal().string(); } }
                        watch(gitDirectory);
                        std::ifstream common(gitDirectory+"/commondir"); std::string shared;
                        if (std::getline(common,shared) && !shared.empty()) { const auto target = std::filesystem::path(shared); gitDirectory = (target.is_absolute() ? target : std::filesystem::path(gitDirectory)/target).lexically_normal().string(); watch(gitDirectory); }
                        struct stat refs{};
                        if (!lstat((gitDirectory+"/refs").c_str(),&refs) && S_ISDIR(refs.st_mode)) watch(gitDirectory+"/refs");
                        if (!lstat((gitDirectory+"/refs/heads").c_str(),&refs) && S_ISDIR(refs.st_mode)) watch(gitDirectory+"/refs/heads");
                        if (op == "indexFiles") continue;
                    }
                    struct stat info{}; if (fstatat(dirfd(dir.get()), name.c_str(), &info, AT_SYMLINK_NOFOLLOW)) continue;
                    const bool link = S_ISLNK(info.st_mode); bool isDirectory = S_ISDIR(info.st_mode);
                    if (link) { struct stat target{}; isDirectory = !fstatat(dirfd(dir.get()), name.c_str(), &target, 0) && S_ISDIR(target.st_mode); }
                    if (op == "indexFiles") {
                        if (!validUtf8(name)) continue;
                        if (isDirectory && !link) { if (++directories > 50000 || pending.size() >= 50000) { truncated = true; continue; } pending.push_back(p); }
                        if (!S_ISREG(info.st_mode)) continue;
                    }
                    if (++entries > (op == "indexFiles" ? 200000 : 10000)) { truncated = true; break; }
                    if (!validUtf8(name)) {
                        std::string display = "[non-UTF-8] "; for (unsigned char c : name) { display += "\\x"; display += "0123456789abcdef"[c >> 4]; display += "0123456789abcdef"[c & 15]; }
                        reply(*r, {{"entry", "1"}, {"name", display}, {"unsupported", "1"}, {"directory", "0"}}); continue;
                    }
                    reply(*r, {{"entry", "1"}, {"path", p}, {"name", name}, {"directory", isDirectory ? "1" : "0"}, {"link", link ? "1" : "0"}});
                }
                if (truncated && entries > 200000) break;
            }
            reply(*r, {{"truncated", truncated ? "1" : "0"}, {"done", "1"}});
        } else if (op == "git") git(*r);
        else throw std::runtime_error("Unsupported WSL operation");
    } catch (const std::exception& error) { if (!stopping_) reply(*r, {{"error", validUtf8(error.what()) ? error.what() : "Linux operation failed"}, {"code", r->cancelled ? "cancelled" : "operationFailed"}, {"done", "1"}}); }
    std::lock_guard<std::mutex> lock(mutex_); uploadBytes_ -= r->size; requests_.erase(number(r->message, "id"));
}
void Server::worker() {
    for (;;) { std::shared_ptr<Request> r; { std::unique_lock<std::mutex> lock(mutex_); condition_.wait(lock, [&] { return stopping_ || !queue_.empty(); }); if (stopping_) return; r = queue_.front(); queue_.pop_front(); } execute(r); }
}
void Server::run() {
    signal(SIGPIPE, SIG_IGN); fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK);
    if (prctl(PR_SET_CHILD_SUBREAPER, 1)) failure("Cannot supervise Git descendants");
    for (int i = 0; i < 3; ++i) workers_.emplace_back([this] { worker(); });
    watcher_ = std::thread([this] { events(); });
    FrameReader reader; char bytes[32768];
    while (!stopping_) { const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes)); if (count < 0) { if (errno == EINTR) continue; failure("Cannot read protocol input"); } if (!count) { reader.finish(); break; } reader.feed(std::string_view(bytes, static_cast<size_t>(count)), [this](Frame f) { accept(std::move(f)); }); }
}
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--stdio") { poppy::wsl::Server server; server.run(); return 0; }
        if (argc == 3 && std::string(argv[1]) == "--stop-terminal") return poppy::wsl::stopTerminal(argv[2]) ? 0 : 1;
        if (argc >= 5 && std::string(argv[1]) == "--terminal") return poppy::wsl::terminal(argc, argv);
        std::cerr << "Usage: poppy-wsl-helper --stdio\n"; return 2;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
