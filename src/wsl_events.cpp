#include "wsl_events.h"
#include "wsl_protocol.h"
#include "ai_status.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace poppy::wsl {
namespace {
using Clock = std::chrono::steady_clock;
volatile sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
struct SocketDirectory {
    std::string directory, socket;
    ~SocketDirectory() { if (!socket.empty()) unlink(socket.c_str()); if (!directory.empty()) rmdir(directory.c_str()); }
};
struct Client { int fd; std::string bytes, reply; Clock::time_point deadline; };
struct Sockets {
    int listener = -1;
    std::vector<Client> clients;
    ~Sockets() { for (const auto& client : clients) close(client.fd); if (listener >= 0) close(listener); }
};
}
int serveTerminalEvents() {
    stopped = 0;
    for (int signal : {SIGTERM, SIGHUP, SIGINT}) std::signal(signal, stop);
    std::signal(SIGPIPE, SIG_IGN);
    char name[] = "/tmp/poppy-events-XXXXXX";
    if (!mkdtemp(name)) throw std::runtime_error("Cannot create notification directory");
    SocketDirectory paths{name, std::string(name) + "/events.sock"};
    Sockets sockets;
    sockets.listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, paths.socket.c_str(), paths.socket.size() + 1);
    if (sockets.listener < 0 || bind(sockets.listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        listen(sockets.listener, 32) || chmod(paths.socket.c_str(), 0666)) throw std::runtime_error("Cannot open notification socket");
    // The private (0700) directory protects the host socket. Docker mounts only
    // this inode, allowing the container's user to differ from the WSL UID.
    if (fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("Cannot configure notification pipe");
    std::deque<std::string> output{encodeFrame({1, encodeMessage({{"socket", paths.socket}})})};
    size_t outputBytes = output.front().size(), offset = 0;
    while (!stopped) {
        std::vector<pollfd> polls{{STDIN_FILENO, POLLIN, 0}, {STDOUT_FILENO, static_cast<short>(output.empty() ? 0 : POLLOUT), 0}, {sockets.listener, POLLIN, 0}};
        for (const auto& client : sockets.clients) polls.push_back({client.fd, static_cast<short>(client.reply.empty() ? POLLIN : POLLOUT), 0});
        if (poll(polls.data(), polls.size(), 50) < 0) { if (errno == EINTR) continue; throw std::runtime_error("Notification poll failed"); }
        if (polls[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char byte; if (::read(STDIN_FILENO, &byte, 1) <= 0) break;
        }
        if (polls[1].revents & (POLLHUP | POLLERR)) break;
        if (!output.empty() && (polls[1].revents & POLLOUT)) {
            const auto count = ::write(STDOUT_FILENO, output.front().data() + offset, output.front().size() - offset);
            if (count > 0) { offset += count; outputBytes -= count; if (offset == output.front().size()) { output.pop_front(); offset = 0; } }
            else if (count < 0 && errno != EAGAIN && errno != EINTR) break;
        }
        for (size_t i = sockets.clients.size(); i-- > 0;) {
            auto& client = sockets.clients[i]; const auto events = polls[i + 3].revents;
            bool done = Clock::now() >= client.deadline || (events & (POLLERR | POLLNVAL));
            if (!done && client.reply.empty() && (events & POLLIN)) {
                char bytes[4096]; const auto count = recv(client.fd, bytes, sizeof(bytes), 0);
                if (count > 0) client.bytes.append(bytes, count);
                else if (!count || (errno != EAGAIN && errno != EINTR)) done = true;
                if (client.bytes.size() > 16384) done = true;
                const auto newline = client.bytes.find('\n');
                if (!done && newline != std::string::npos) {
                    try {
                        if (newline + 1 != client.bytes.size()) throw std::runtime_error("Trailing notification data");
                        const auto event = decodeMessage(std::string_view(client.bytes).substr(0, newline));
                        const auto kind = field(event, "type"), body = field(event, "body");
                        if (event.size() != 2 || (kind != "status" && kind != "activity") ||
                            !parseAiEvent("00000000000000000000000000000000", kind == "activity", body)) throw std::runtime_error("Invalid notification");
                        auto frame = encodeFrame({1, encodeMessage(event)});
                        if (output.size() >= 1024 || outputBytes + frame.size() > 1024 * 1024) client.reply = "BUSY\n";
                        else { outputBytes += frame.size(); output.push_back(std::move(frame)); client.reply = "OK\n"; }
                        client.bytes.clear();
                    } catch (...) { done = true; }
                }
            }
            if (!done && !client.reply.empty() && (events & POLLOUT)) {
                const auto count = send(client.fd, client.reply.data(), client.reply.size(), MSG_NOSIGNAL);
                if (count > 0) { client.reply.erase(0, count); if (client.reply.empty()) done = true; }
                else if (count < 0 && errno != EAGAIN && errno != EINTR) done = true;
            }
            if (events & POLLHUP) done = true;
            if (done) { close(client.fd); sockets.clients.erase(sockets.clients.begin() + i); }
        }
        if (polls[2].revents & POLLIN) {
            const auto fd = accept4(sockets.listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd >= 0) {
                if (sockets.clients.size() >= 32) close(fd);
                else sockets.clients.push_back({fd, {}, {}, Clock::now() + std::chrono::seconds(2)});
            }
        }
    }
    return 0;
}
}
