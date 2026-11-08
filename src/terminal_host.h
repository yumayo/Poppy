#pragma once
#include "win.h"
#include "terminal_wire.h"

namespace poppy {
struct TerminalHostLaunch {
    fs::path directory;
    std::vector<std::wstring> command, eventCommand, stopCommand;
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    std::string eventInput;
    bool requireEvents = false;
    int columns = 80, rows = 24;
};
struct TerminalHostUpdate {
    bool snapshot = false, running = false, relay = false;
    size_t scroll = 0;
    std::string data, status, relayError, diagnostic;
};
namespace terminalWire {
enum class Op : uint8_t { Start = 1, Attach, Poll, Input, Resize, Detach, Stop, Update, Ack, Error };
struct Packet { Op op; std::string data; };
std::wstring pipeName(const std::string& session);
void writePacket(HANDLE pipe, Op op, std::string_view bytes);
Packet readPacket(HANDLE pipe, const std::function<bool()>& cancelled = {});
std::string encodeLaunch(const TerminalHostLaunch& launch);
TerminalHostLaunch decodeLaunch(std::string_view bytes);
std::string encodeUpdate(const TerminalHostUpdate& update);
TerminalHostUpdate decodeUpdate(std::string_view bytes);
}
// Used only by the terminal's worker. A successful detach transfers ownership
// to the host for a bounded interval; an ordinary close waits for all cleanup.
class TerminalHostClient {
public:
    ~TerminalHostClient();
    bool attach(const std::string& session, TerminalHostUpdate& initial);
    TerminalHostUpdate start(const std::string& session, const TerminalHostLaunch& launch);
    TerminalHostUpdate poll();
    void input(std::string_view bytes);
    void resize(int columns, int rows);
    void close(bool preserve = false, size_t scroll = 0);
    bool connected() const { return static_cast<bool>(pipe_); }
private:
    bool connect(const std::string& session);
    terminalWire::Packet response();
    Handle pipe_, process_;
};
}
