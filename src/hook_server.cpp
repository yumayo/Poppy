#include <winsock2.h>
#include <ws2tcpip.h>
#include "hook_server.h"
#include "render.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace poppy {
std::string newSessionId() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot allocate terminal identity");
    std::string result; constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(guid); ++i) { const auto byte = reinterpret_cast<const unsigned char*>(&guid)[i]; result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
HookServer::~HookServer() { stop(); }
bool HookServer::start(HWND target) {
    target_ = target;
    WSADATA data{}; if (WSAStartup(MAKEWORD(2,2), &data)) { error_ = L"Winsock unavailable"; return false; } winsock_ = true;
    const auto bindEnv = std::getenv("POPPY_HOOK_BIND"), portEnv = std::getenv("POPPY_HOOK_PORT"), hostEnv = std::getenv("POPPY_HOOK_HOST");
    const std::string bind = bindEnv ? bindEnv : "127.0.0.1";
    const std::string port = portEnv ? portEnv : "0";
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos || std::strtoul(port.c_str(), nullptr, 10) > 65535) { error_ = L"Invalid POPPY_HOOK_PORT"; return false; }
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<u_short>(std::strtoul(port.c_str(), nullptr, 10)));
    if (inet_pton(AF_INET, bind.c_str(), &address.sin_addr) != 1) { error_ = L"POPPY_HOOK_BIND must be an IPv4 address"; return false; }
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE; setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (listener_ == INVALID_SOCKET || ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener_, 32)) { error_ = L"Cannot listen for AI hooks: " + std::to_wstring(WSAGetLastError()); stop(); return false; }
    int size = sizeof(address); getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
    u_long nonblocking = 1; ioctlsocket(listener_, FIONBIO, &nonblocking);
    const std::string host = hostEnv ? hostEnv : bind == "0.0.0.0" ? "127.0.0.1" : bind;
    if (host.empty() || host.find_first_of("\r\n /\\\"") != std::string::npos) { error_ = L"Invalid POPPY_HOOK_HOST"; stop(); return false; }
    url_ = "http://" + host + ":" + std::to_string(ntohs(address.sin_port));
    stopping_ = false; thread_.start([this] { serve(); }); return true;
}
void HookServer::stop() {
    stopping_ = true; thread_.join();
    if (listener_ != INVALID_SOCKET) { closesocket(listener_); listener_ = INVALID_SOCKET; }
    if (winsock_) { WSACleanup(); winsock_ = false; }
}
void HookServer::add(const std::string& id) { std::lock_guard<std::mutex> lock(mutex_); ids_.insert(id); }
void HookServer::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_); ids_.erase(id);
    events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const AiEvent& event) { return event.sessionId == id; }), events_.end());
}
std::vector<AiEvent> HookServer::drain() {
    std::lock_guard<std::mutex> lock(mutex_); std::vector<AiEvent> result;
    while (!events_.empty()) { if (ids_.count(events_.front().sessionId)) result.push_back(std::move(events_.front())); events_.pop_front(); }
    return result;
}
void HookServer::serve() {
    struct Client { SOCKET socket; std::string bytes; ULONGLONG deadline; };
    std::vector<Client> clients;
    const auto response = [](SOCKET socket, int code) {
        const std::string text = "HTTP/1.1 " + std::to_string(code) + (code == 204 ? " No Content" : " Error") + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(socket, text.data(), static_cast<int>(text.size()), 0);
    };
    while (!stopping_) {
        std::vector<WSAPOLLFD> polls{{listener_, POLLRDNORM, 0}};
        for (const auto& client : clients) polls.push_back({client.socket, POLLRDNORM, 0});
        WSAPoll(polls.data(), static_cast<ULONG>(polls.size()), 50);
        for (size_t i = clients.size(); i-- > 0;) {
            auto& client = clients[i]; bool done = false; int code = 0;
            if (polls[i+1].revents & POLLRDNORM) {
                char buffer[4096]; const int count = recv(client.socket, buffer, sizeof(buffer), 0);
                if (count > 0) client.bytes.append(buffer, count); else if (count == 0 || WSAGetLastError() != WSAEWOULDBLOCK) done = true;
            }
            if (client.bytes.size() > 16384) code = 413;
            const auto split = client.bytes.find("\r\n\r\n");
            if (!code && split != std::string::npos) {
                const auto first = client.bytes.find("\r\n");
                const auto line = client.bytes.substr(0, first);
                const auto a = line.find(' '), b = line.find(' ', a == std::string::npos ? 0 : a+1);
                std::map<std::string, std::string> headers;
                if (a == std::string::npos || b == std::string::npos || (line.substr(b+1) != "HTTP/1.1" && line.substr(b+1) != "HTTP/1.0")) code = 400;
                for (size_t p = first + 2; !code && p < split;) {
                    const auto end = client.bytes.find("\r\n", p), colon = client.bytes.find(':', p);
                    if (end == std::string::npos || colon >= end) { code = 400; break; }
                    auto key = client.bytes.substr(p, colon-p), value = client.bytes.substr(colon+1, end-colon-1);
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
                    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
                    if (!headers.emplace(std::move(key), std::move(value)).second) code = 400;
                    p = end + 2;
                }
                const auto length = headers.find("content-length");
                size_t size = 0;
                if (!code) {
                    if (headers.count("transfer-encoding") || length == headers.end() || length->second.empty() || length->second.size() > 5 || length->second.find_first_not_of("0123456789") != std::string::npos) code = 400;
                    else size = std::strtoul(length->second.c_str(), nullptr, 10);
                    if (size > 8192 || split > 8192) code = 413;
                    const auto type = headers.find("content-type");
                    if (type == headers.end() || (type->second != "application/json" && type->second.rfind("application/json;", 0) != 0)) code = 415;
                }
                if (!code && client.bytes.size() >= split + 4 + size) {
                    const auto method = line.substr(0, a), path = line.substr(a+1, b-a-1);
                    const std::string prefix = "/tasks/sessions/";
                    if (path.rfind(prefix, 0) != 0 || path.size() < prefix.size()+32) code = 404;
                    else {
                        const auto id = path.substr(prefix.size(), 32), suffix = path.substr(prefix.size()+32);
                        const bool activity = method == "PUT" && suffix == "/activity";
                        if (!activity && !(method == "POST" && suffix == "/status")) code = 404;
                        else if (const auto event = parseAiEvent(id, activity, std::string_view(client.bytes).substr(split+4, size))) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (!ids_.count(id)) code = 404;
                            else if (events_.size() >= 1024) code = 503;
                            else { events_.push_back(*event); PostMessageW(target_, wmAiEvent, 0, 0); code = 204; }
                        } else code = 400;
                    }
                }
            } else if (!code && client.bytes.size() > 8192) code = 413;
            if (!code && GetTickCount64() >= client.deadline) code = 408;
            if (code || done || (polls[i+1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                if (code) response(client.socket, code);
                closesocket(client.socket); clients.erase(clients.begin()+static_cast<std::ptrdiff_t>(i));
            }
        }
        if (polls.front().revents & POLLRDNORM) {
            const auto socket = accept(listener_, nullptr, nullptr);
            if (socket != INVALID_SOCKET) {
                if (clients.size() >= 32) { response(socket, 503); closesocket(socket); }
                else { u_long nonblocking = 1; ioctlsocket(socket, FIONBIO, &nonblocking); clients.push_back({socket, {}, GetTickCount64()+2000}); }
            }
        }
    }
    for (const auto& client : clients) closesocket(client.socket);
}
void drawAiStatus(Renderer& dc, RECT rect, AiState state, UINT dpi, unsigned frame) {
    const int size = MulDiv(14, dpi, 96), x = (rect.left+rect.right)/2, y = (rect.top+rect.bottom)/2;
    const float unit = static_cast<float>(dpi)/96;
    const COLORREF color = state == AiState::Running || state == AiState::Completed ? accent : state == AiState::Idle || state == AiState::Unknown ? muted : RGB(224,192,128);
    const auto line = [&](int x1,int y1,int x2,int y2) { dc.line(x+x1*unit,y+y1*unit,x+x2*unit,y+y2*unit,color,1.6f*unit); };
    if (state == AiState::Running) {
        dc.ellipse({x-size/2,y-size/2,x+size/2,y+size/2}, muted, unit);
        const float angle = frame * 0.35f;
        dc.line(static_cast<float>(x),static_cast<float>(y),x+std::cos(angle)*size/2,y+std::sin(angle)*size/2,color,2*unit);
    } else if (state == AiState::Completed) { line(-6,0,-2,4); line(-2,4,6,-5); }
    else if (state == AiState::Paused) { line(-3,-6,-3,6); line(3,-6,3,6); }
    else if (state == AiState::Idle) dc.fill({x-1,y-1,x+2,y+2},color);
    else if (state == AiState::Unknown) dc.ellipse({x-size/2,y-size/2,x+size/2,y+size/2},color,unit);
    else dc.label(rect, state == AiState::Failed ? L"!" : L"?", color, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}
}
