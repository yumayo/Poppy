#include "terminal_host.h"
#include "ai_status.h"
#include <algorithm>

namespace poppy {
namespace terminalWire {
namespace {
void transfer(HANDLE pipe, bool write, char* data, size_t size, const std::function<bool()>& cancelled = {}, DWORD timeout = 15000) {
    const auto deadline = GetTickCount64() + timeout;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw std::runtime_error("Cannot create terminal pipe event");
    while (size) {
        OVERLAPPED operation{}; operation.hEvent = event.value; ResetEvent(event.value);
        DWORD count = 0;
        const auto length = static_cast<DWORD>(std::min<size_t>(size, 65536));
        const BOOL complete = write ? WriteFile(pipe, data, length, &count, &operation) : ReadFile(pipe, data, length, &count, &operation);
        if (!complete) {
            if (GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Terminal connection closed: " + utf8(winError()));
            while (WaitForSingleObject(event.value, 50) == WAIT_TIMEOUT) {
                if ((timeout != INFINITE && GetTickCount64() >= deadline) || (cancelled && cancelled())) {
                    CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &count, TRUE);
                    throw std::runtime_error("Terminal connection timed out or its owner exited");
                }
            }
            if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) throw std::runtime_error("Terminal connection closed: " + utf8(winError()));
        }
        if (!count) throw std::runtime_error("Terminal connection closed");
        data += count; size -= count;
    }
}
void strings(Writer& out, const std::vector<std::wstring>& values) {
    if (values.size() > 256) throw std::runtime_error("Too many terminal command arguments");
    out.number(values.size()); for (const auto& value : values) out.string(utf8(value));
}
std::vector<std::wstring> strings(Reader& in) {
    std::vector<std::wstring> values; const auto count = in.bounded(0, 256);
    for (uint32_t i = 0; i < count; ++i) { auto value = in.string(65536); if (value.find('\0') != value.npos) in.invalid(); values.push_back(wide(value)); }
    return values;
}
}
std::wstring pipeName(const std::string& session) {
    if (!validNotificationId(session)) throw std::runtime_error("Invalid terminal session identity");
    return L"\\\\.\\pipe\\Poppy.Terminal.v1." + wide(session);
}
void writePacket(HANDLE pipe, Op op, std::string_view bytes) {
    if (bytes.size() > maxPacket) throw std::runtime_error("Terminal packet is too large");
    Writer header; header.number(static_cast<uint8_t>(op), 1); header.number(bytes.size());
    transfer(pipe, true, header.bytes.data(), header.bytes.size());
    transfer(pipe, true, const_cast<char*>(bytes.data()), bytes.size());
}
Packet readPacket(HANDLE pipe, const std::function<bool()>& cancelled) {
    char header[5]; transfer(pipe, false, header, sizeof(header), cancelled, cancelled ? INFINITE : 15000); Reader in(std::string_view(header, sizeof(header)));
    const auto op = in.number(1), size = in.number();
    if (op < static_cast<unsigned>(Op::Start) || op > static_cast<unsigned>(Op::Error) || size > maxPacket) in.invalid();
    Packet result{static_cast<Op>(op), std::string(static_cast<size_t>(size), '\0')};
    transfer(pipe, false, result.data.data(), result.data.size(), cancelled); return result;
}
std::string encodeLaunch(const TerminalHostLaunch& launch) {
    Writer out; out.number(version); out.string(launch.directory.u8string()); out.number(launch.columns); out.number(launch.rows); out.flag(launch.requireEvents);
    strings(out, launch.command); strings(out, launch.eventCommand); strings(out, launch.stopCommand);
    out.number(launch.environment.size()); for (const auto& value : launch.environment) { out.string(utf8(value.first)); out.string(utf8(value.second)); }
    out.string(launch.eventInput); return std::move(out.bytes);
}
TerminalHostLaunch decodeLaunch(std::string_view bytes) {
    Reader in(bytes); if (in.number() != version) throw std::runtime_error("Incompatible terminal host protocol");
    TerminalHostLaunch launch; const auto path = in.string(131072); if (path.empty() || path.find('\0') != path.npos) in.invalid(); launch.directory = fs::u8path(path);
    launch.columns = static_cast<int>(in.bounded(2, 500)); launch.rows = static_cast<int>(in.bounded(1, 200)); launch.requireEvents = in.flag();
    launch.command = strings(in); launch.eventCommand = strings(in); launch.stopCommand = strings(in);
    const auto count = in.bounded(0, 256);
    for (uint32_t i = 0; i < count; ++i) {
        auto name = in.string(65536), value = in.string(65536);
        if (name.empty() || name.find_first_of("=\0", 0, 2) != name.npos || value.find('\0') != value.npos) in.invalid();
        launch.environment.emplace_back(wide(name), wide(value));
    }
    launch.eventInput = in.string(16 * 1024 * 1024); in.finish(); return launch;
}
std::string encodeUpdate(const TerminalHostUpdate& update) {
    Writer out; out.flag(update.snapshot); out.flag(update.running); out.flag(update.relay); out.number(update.scroll);
    out.string(update.data); out.string(update.status); out.string(update.relayError); out.string(update.diagnostic); return std::move(out.bytes);
}
TerminalHostUpdate decodeUpdate(std::string_view bytes) {
    Reader in(bytes); TerminalHostUpdate update; update.snapshot = in.flag(); update.running = in.flag(); update.relay = in.flag(); update.scroll = in.bounded(0, 1000);
    update.data = in.string(8 * 1024 * 1024); update.status = in.string(8192); update.relayError = in.string(65536); update.diagnostic = in.string(128);
    in.finish(); return update;
}
}
using terminalWire::Op;
TerminalHostClient::~TerminalHostClient() { try { close(); } catch (...) { } }
bool TerminalHostClient::connect(const std::string& session) {
    pipe_.reset(CreateFileW(terminalWire::pipeName(session).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!pipe_) {
        const auto error = GetLastError(); if (error == ERROR_FILE_NOT_FOUND) return false;
        throw std::runtime_error("Cannot reconnect terminal: " + utf8(winError(error)));
    }
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe_.value, &pid)) throw std::runtime_error("Cannot identify terminal host");
    if (process_ && GetProcessId(process_.value) != pid) throw std::runtime_error("Terminal session is already owned by another host");
    if (!process_) process_.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process_) throw std::runtime_error("Cannot monitor terminal host");
    return true;
}
terminalWire::Packet TerminalHostClient::response() {
    // Relay/WSL cleanup can legitimately take longer than an I/O timeout.
    // Keep the UI worker waiting while monitoring the owning host process.
    auto packet = terminalWire::readPacket(pipe_.value, [this] { return process_ && WaitForSingleObject(process_.value, 0) != WAIT_TIMEOUT; });
    if (packet.op == Op::Error) throw std::runtime_error(packet.data);
    return packet;
}
bool TerminalHostClient::attach(const std::string& session, TerminalHostUpdate& initial) {
    if (!connect(session)) return false;
    terminalWire::Writer request; request.number(terminalWire::version);
    terminalWire::writePacket(pipe_.value, Op::Attach, request.bytes);
    const auto packet = response(); if (packet.op != Op::Update) throw std::runtime_error("Invalid terminal attachment response");
    initial = terminalWire::decodeUpdate(packet.data); return true;
}
TerminalHostUpdate TerminalHostClient::start(const std::string& session, const TerminalHostLaunch& launch) {
    close();
    const auto executable = applicationDirectory() / L"PoppyTerminalHost.exe";
    auto command = processCommandLine({executable.native(), wide(session)});
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, launch.directory.c_str(), &startup, &info))
        throw std::runtime_error("Cannot start terminal host: " + utf8(winError()));
    process_.reset(info.hProcess); Handle thread(info.hThread);
    const auto deadline = GetTickCount64() + 10000;
    while (!connect(session)) {
        if (WaitForSingleObject(process_.value, 10) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) throw std::runtime_error("Terminal host did not start");
    }
    terminalWire::writePacket(pipe_.value, Op::Start, terminalWire::encodeLaunch(launch));
    const auto packet = response(); if (packet.op != Op::Update) throw std::runtime_error("Invalid terminal startup response");
    return terminalWire::decodeUpdate(packet.data);
}
TerminalHostUpdate TerminalHostClient::poll() {
    terminalWire::writePacket(pipe_.value, Op::Poll, {});
    const auto packet = response(); if (packet.op != Op::Update) throw std::runtime_error("Invalid terminal update");
    return terminalWire::decodeUpdate(packet.data);
}
void TerminalHostClient::input(std::string_view bytes) { terminalWire::writePacket(pipe_.value, Op::Input, bytes); }
void TerminalHostClient::resize(int columns, int rows) { terminalWire::Writer out; out.number(columns); out.number(rows); terminalWire::writePacket(pipe_.value, Op::Resize, out.bytes); }
void TerminalHostClient::close(bool preserve, size_t scroll) {
    std::exception_ptr failure;
    if (pipe_) {
        try {
            terminalWire::Writer out; if (preserve) out.number(std::min<size_t>(scroll, 1000));
            terminalWire::writePacket(pipe_.value, preserve ? Op::Detach : Op::Stop, out.bytes);
            if (response().op != Op::Ack) throw std::runtime_error("Terminal host did not acknowledge shutdown");
        } catch (...) { failure = std::current_exception(); preserve = false; }
        pipe_.reset();
    }
    if (process_ && !preserve) {
        WaitForSingleObject(process_.value, INFINITE);
        DWORD code = 0;
        if (!failure && GetExitCodeProcess(process_.value, &code) && code != 0)
            failure = std::make_exception_ptr(std::runtime_error("Terminal host could not confirm complete cleanup of its Windows/WSL processes"));
    }
    process_.reset();
    if (failure) std::rethrow_exception(failure);
}
}
