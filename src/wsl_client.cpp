#include "project_provider.h"
#include "wsl_protocol.h"
#include "content_search_protocol.h"
#include "wsl_bootstrap.h"
#include "text.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>

namespace poppy {
namespace {
using namespace wsl;
std::string bundledWslHelper() {
    std::wstring executable(32768, 0); const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())); if (!count || count >= executable.size()) throw std::runtime_error("Cannot locate Poppy's WSL helper"); executable.resize(count);
    const auto helper = fs::path(executable).parent_path() / L"wsl" / L"poppy-wsl-helper";
    std::ifstream file(helper, std::ios::binary | std::ios::ate); if (!file) throw std::runtime_error("The Linux helper is missing. Place the release's wsl folder beside Poppy.exe.");
    const auto length = file.tellg(); if (length <= 0 || length > 32 * 1024 * 1024) throw std::runtime_error("Invalid Linux helper size");
    std::string binary(static_cast<size_t>(length), 0); file.seekg(0); if (!file.read(binary.data(), static_cast<std::streamsize>(binary.size()))) throw std::runtime_error("Cannot read Linux helper");
    return binary;
}
struct Response { Message metadata; std::vector<Message> entries; std::string bytes; };
struct Pending {
    Response response;
    std::string project, generation;
    bool done = false, body = false, last = false;
    uint64_t size = 0;
    size_t entryBytes = 0;
    std::function<void(const Message&)> receive;
};
class Connection {
public:
    explicit Connection(ProjectLocation location) : location_(std::move(location)) {}
    ~Connection() { stop(); }
    Response call(Message message, const std::string* bytes = nullptr, const std::function<bool()>& cancelled = {}, DWORD timeout = 10000,
        std::function<void(const Message&)> receive = {});
    void ensure();
    void reconnect();
    ConnectionState state() const { return state_.load(); }
    std::wstring error() const { std::lock_guard<std::mutex> lock(mutex_); return error_; }
    uint64_t revision() const { return revision_; }
    std::vector<std::wstring> terminal(const ResourceId& path, const std::string& token, const std::wstring& tool) const;
    std::vector<std::wstring> terminalEvents() const;
    bool stopTerminal(const std::string& token) const;
private:
    void start();
    void stop();
    void send(Frame frame, DWORD timeout = 10000);
    void raw(std::string bytes, DWORD timeout);
    void received(Frame frame);
    void fail(const std::wstring& error);
    std::vector<std::wstring> command() const;
    ProjectLocation location_;
    mutable std::mutex mutex_;
    std::mutex startMutex_, uploadMutex_;
    std::condition_variable condition_;
    std::map<uint64_t, std::shared_ptr<Pending>> pending_;
    struct ProjectWatch { ResourceId root; ProviderChanges changes; };
    std::map<std::string, ProjectWatch> projectWatches_;
    bool watchLimited_ = false;
    std::deque<std::string> writes_;
    size_t writeBytes_ = 0;
    size_t responseBytes_ = 0;
    Handle input_, output_, diagnostics_, process_, job_;
    NativeThread reader_, writer_;
    std::atomic<bool> stopping_{false};
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    std::atomic<uint64_t> nextId_{1}, revision_{0};
    std::wstring error_;
    std::string diagnostic_, helperPath_, shell_, uid_;
    friend class WslProvider;
};
std::vector<std::wstring> Connection::command() const {
    std::vector<std::wstring> args{wslExecutable(), L"--distribution", wide(location_.distro)};
    if (!location_.user.empty()) { args.push_back(L"--user"); args.push_back(wide(location_.user)); }
    args.push_back(L"--exec"); return args;
}
void Connection::fail(const std::wstring& error) {
    { std::lock_guard<std::mutex> lock(mutex_); error_ = error; if (!diagnostic_.empty() && validUtf8(diagnostic_)) error_ += L"\n" + wide(diagnostic_); state_ = ConnectionState::Unavailable; stopping_ = true; for (auto& item : pending_) { item.second->response.metadata["error"] = utf8(error_); item.second->done = true; } }
    ++revision_; condition_.notify_all();
    if (job_) TerminateJobObject(job_.value, 1);
}
void Connection::stop() {
    if (state_ == ConnectionState::Ready && !stopping_) {
        try { send({1, encodeMessage({{"op", "shutdown"}})}, 500); if (process_) WaitForSingleObject(process_.value, 500); } catch (const std::exception&) {}
    }
    stopping_ = true; condition_.notify_all();
    if (process_) TerminateJobObject(job_.value, 1);
    if (writer_.joinable()) while (WaitForSingleObject(writer_.native_handle(), 0) == WAIT_TIMEOUT) { CancelSynchronousIo(writer_.native_handle()); WaitForSingleObject(writer_.native_handle(), 10); }
    writer_.join(); reader_.join();
    input_.reset(); output_.reset(); diagnostics_.reset(); process_.reset(); job_.reset();
    { std::lock_guard<std::mutex> lock(mutex_); writes_.clear(); writeBytes_ = 0; }
}
void Connection::reconnect() { std::lock_guard<std::mutex> lock(startMutex_); if (state_ == ConnectionState::Unavailable || state_ == ConnectionState::Disconnected) { stop(); state_ = ConnectionState::Disconnected; } }
void Connection::start() {
    const auto binary = bundledWslHelper(); const auto hash = sha256(binary);
    const auto distributions = installedWslDistributions();
    if (std::find(distributions.begin(), distributions.end(), location_.distro) == distributions.end()) throw std::runtime_error("The WSL distribution is no longer registered: " + location_.distro);
    auto args = command(); args.insert(args.end(), {L"/bin/sh", L"-c", wide(bootstrap), L"poppy-bootstrap", wide(helperVersion), wide(hash), std::to_wstring(binary.size())});
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) throw std::runtime_error(utf8(winError()));
    Handle childInput(inRead); input_.reset(inWrite);
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) throw std::runtime_error(utf8(winError()));
    output_.reset(outRead); Handle childOutput(outWrite);
    if (!CreatePipe(&errRead, &errWrite, &sa, 0)) throw std::runtime_error(utf8(winError()));
    diagnostics_.reset(errRead); Handle childError(errWrite);
    for (const auto handle : {input_.value, output_.value, diagnostics_.value}) if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error(utf8(winError()));
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size); std::vector<unsigned char> attributes(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data()); if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) throw std::runtime_error(utf8(winError()));
    struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST list; ~Delete() { DeleteProcThreadAttributeList(list); } } cleanup{list};
    HANDLE inherited[]{childInput.value, childOutput.value, childError.value}; if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) throw std::runtime_error(utf8(winError()));
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput = childInput.value; startup.StartupInfo.hStdOutput = childOutput.value; startup.StartupInfo.hStdError = childError.value; startup.lpAttributeList = list;
    job_.reset(CreateJobObjectW(nullptr, nullptr)); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) throw std::runtime_error(utf8(winError()));
    auto line = processCommandLine(args);
    PROCESS_INFORMATION pi{}; const auto cwd = windowsProcessDirectory();
    if (!CreateProcessW(args[0].c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.c_str(), &startup.StartupInfo, &pi)) throw std::runtime_error(utf8(winError()));
    process_.reset(pi.hProcess); Handle thread(pi.hThread);
    if (!AssignProcessToJobObject(job_.value, process_.value)) { TerminateProcess(process_.value, 1); throw std::runtime_error(utf8(winError())); }
    stopping_ = false; diagnostic_.clear(); watchLimited_ = false; ResumeThread(thread.value); childInput.reset(); childOutput.reset(); childError.reset();
    reader_.start([this] {
        FrameReader frames;
        try {
            while (!stopping_) {
                char bytes[32768]; DWORD count = 0, available = 0;
                if (PeekNamedPipe(diagnostics_.value, nullptr, 0, nullptr, &available, nullptr) && available) {
                    if (ReadFile(diagnostics_.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) && count) { std::lock_guard<std::mutex> lock(mutex_); diagnostic_.append(bytes, std::min<size_t>(count, 8192 - diagnostic_.size())); }
                }
                if (!PeekNamedPipe(output_.value, nullptr, 0, nullptr, &available, nullptr)) { frames.finish(); throw std::runtime_error("WSL helper disconnected. Use Reconnect or F5 to retry."); }
                if (available) { if (!ReadFile(output_.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) || !count) throw std::runtime_error("WSL helper disconnected"); frames.feed(std::string_view(bytes, count), [this](Frame frame) { received(std::move(frame)); }); }
                else if (WaitForSingleObject(process_.value, 15) == WAIT_OBJECT_0) { DWORD remaining = 0; if (!PeekNamedPipe(output_.value, nullptr, 0, nullptr, &remaining, nullptr) || !remaining) { frames.finish(); throw std::runtime_error("WSL helper exited. Use Reconnect or F5 to retry."); } }
            }
        } catch (const std::exception& error) { if (!stopping_) fail(wide(error.what())); }
    });
    writer_.start([this] {
        for (;;) {
            std::string bytes;
            { std::unique_lock<std::mutex> lock(mutex_); condition_.wait(lock, [&] { return stopping_ || !writes_.empty(); }); if (stopping_) return; bytes = std::move(writes_.front()); writes_.pop_front(); writeBytes_ -= bytes.size(); condition_.notify_all(); }
            size_t offset = 0;
            while (offset < bytes.size() && !stopping_) { DWORD count = 0; if (!WriteFile(input_.value, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr) || !count) { if (!stopping_) fail(L"WSL helper input disconnected"); return; } offset += count; }
        }
    });
    for (size_t offset = 0; offset < binary.size(); offset += maxChunk) raw(binary.substr(offset, maxChunk), 10000);
}
void Connection::raw(std::string bytes, DWORD timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return stopping_ || writeBytes_ + bytes.size() <= 1024 * 1024; })) { lock.unlock(); fail(L"WSL helper stopped accepting input"); throw std::runtime_error("WSL helper input timed out"); }
    if (stopping_) throw std::runtime_error(utf8(error_));
    writeBytes_ += bytes.size(); writes_.push_back(std::move(bytes)); condition_.notify_all();
}
void Connection::send(Frame frame, DWORD timeout) { raw(encodeFrame(frame), timeout); }
void Connection::ensure() {
    std::lock_guard<std::mutex> lock(startMutex_); if (state_ == ConnectionState::Ready) return;
    if (state_ == ConnectionState::Unavailable) throw std::runtime_error(utf8(error()));
    state_ = ConnectionState::Connecting;
    try {
        start();
        const auto hello = call({{"op", "hello"}, {"protocolVersion", std::to_string(protocolVersion)}, {"projectId", "hello"}, {"generation", "0"}});
        if (field(hello.metadata, "protocolVersion") != std::to_string(protocolVersion) || field(hello.metadata, "helperVersion") != helperVersion || field(hello.metadata, "architecture") != "x86_64") throw std::runtime_error("Unsupported WSL helper protocol, version or architecture");
        number(hello.metadata, "uid");
        const auto capabilities = ',' + field(hello.metadata, "capabilities") + ',';
        for (const auto& capability : {"list", "read", "save", "create", "mkdir", "rename", "index", "git", "inotify", "terminal", "search"}) if (capabilities.find(',' + std::string(capability) + ',') == capabilities.npos) throw std::runtime_error("The WSL helper is missing a required capability");
        const auto homePath = field(hello.metadata, "home");
        { std::lock_guard<std::mutex> stateLock(mutex_); shell_ = field(hello.metadata, "shell"); uid_ = field(hello.metadata, "uid"); helperPath_ = field(hello.metadata, "executable"); }
        if (homePath.empty() || homePath[0] != '/' || shell_.empty() || shell_[0] != '/' || helperPath_.empty() || helperPath_[0] != '/' || homePath.find('\0') != homePath.npos || shell_.find('\0') != shell_.npos || helperPath_.find('\0') != helperPath_.npos) throw std::runtime_error("Invalid WSL handshake paths");
        state_ = ConnectionState::Ready;
    } catch (const std::exception& error) { fail(wide(error.what())); throw; }
}
void Connection::received(Frame frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (frame.kind == 1) {
        auto message = decodeMessage(frame.payload);
        const auto event = field(message, "event");
        if (event == "watchLimited") {
            watchLimited_ = true;
            for (auto& project : projectWatches_) project.second.changes.watchLimited = true;
            return;
        }
        if (event == "rescanRequired") {
            const auto path = field(message, "path");
            if (!path.empty() && (path.front() != '/' || path.find('\0') != path.npos || !validUtf8(path))) throw std::runtime_error("Invalid WSL change path");
            for (auto& project : projectWatches_) {
                const auto& root = project.second.root; const auto resource = path.empty() ? root : root.at(path);
                if (!path.empty() && !root.contains(resource) && !resource.contains(root)) continue;
                auto& changes = project.second.changes;
                changes.changed = true; changes.files |= field(message, "files") == "1";
                if (field(message, "content") == "1" && !changes.allContent) {
                    if (path.empty() || changes.contentPaths.size() >= 256) { changes.allContent = true; changes.contentPaths.clear(); }
                    else changes.contentPaths.push_back(resource);
                }
            }
            return;
        }
        const auto found = pending_.find(number(message, "id")); if (found == pending_.end()) throw std::runtime_error("Unexpected WSL response ID"); auto& pending = *found->second;
        if (pending.done || field(message, "projectId") != pending.project || field(message, "generation") != pending.generation) throw std::runtime_error("WSL response authority/generation mismatch");
        if (field(message, "entry") == "1") {
            if (pending.receive) { const auto receive = pending.receive; lock.unlock(); receive(message); return; }
            pending.entryBytes += frame.payload.size(); responseBytes_ += frame.payload.size(); if (pending.response.entries.size() >= 200000 || responseBytes_ > 64 * 1024 * 1024) throw std::runtime_error("WSL listing exceeds the receive limit"); pending.response.entries.push_back(std::move(message));
        } else {
            if (field(message, "body") == "1") { if (pending.body) throw std::runtime_error("Duplicate WSL file header"); pending.body = true; pending.size = number(message, "size"); if (pending.size > maxFileBytes) throw std::runtime_error("WSL file exceeds the 16 MiB receive limit"); }
            for (const auto& item : message) pending.response.metadata[item.first] = item.second;
            if (field(message, "done") == "1") { if (pending.body && !pending.last && field(message, "error").empty()) throw std::runtime_error("Missing final WSL body chunk"); pending.done = true; condition_.notify_all(); }
        }
    } else {
        const auto chunk = decodeChunk(frame); const auto found = pending_.find(chunk.id); if (found == pending_.end()) throw std::runtime_error("Unexpected WSL body ID"); auto& pending = *found->second;
        if (pending.done || !pending.body || pending.last || chunk.offset != pending.response.bytes.size() || chunk.bytes.size() > pending.size - pending.response.bytes.size()) throw std::runtime_error("Invalid WSL body offset/size");
        pending.response.bytes += chunk.bytes; responseBytes_ += chunk.bytes.size(); if (responseBytes_ > 64 * 1024 * 1024) throw std::runtime_error("WSL responses exceed the connection receive limit"); if (chunk.last) { if (pending.response.bytes.size() != pending.size) throw std::runtime_error("Truncated WSL body"); pending.last = true; }
    }
}
Response Connection::call(Message message, const std::string* bytes, const std::function<bool()>& cancelled, DWORD timeout,
    std::function<void(const Message&)> receive) {
    const bool search = field(message, "op") == "searchContent";
    const auto id = nextId_++; message["id"] = std::to_string(id); auto pending = std::make_shared<Pending>(); pending->project = field(message, "projectId"); pending->generation = field(message, "generation");
    pending->receive = std::move(receive);
    const auto deadline = GetTickCount64() + timeout;
    { std::lock_guard<std::mutex> lock(mutex_); if (pending_.size() >= 128) throw std::runtime_error("WSL request limit reached"); pending_[id] = pending; }
    try {
        { std::lock_guard<std::mutex> lock(uploadMutex_); send({1, encodeMessage(message)}); if (bytes) { size_t offset = 0; do { const auto now = GetTickCount64(); if (now >= deadline) { fail(L"WSL upload timed out. Verify the file before retrying."); throw std::runtime_error("WSL upload timed out"); } const size_t count = std::min(maxChunk, bytes->size() - offset); send(dataFrame(id, offset, offset + count == bytes->size(), std::string_view(*bytes).substr(offset, count)), static_cast<DWORD>(deadline - now)); offset += count; } while (offset < bytes->size()); } }
        bool cancelling = false, searchTimedOut = false; ULONGLONG cancelledAt = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!pending->done && !stopping_) {
            condition_.wait_for(lock, std::chrono::milliseconds(30));
            searchTimedOut = search && GetTickCount64() > deadline;
            if (!cancelling && ((cancelled && cancelled()) || searchTimedOut)) { cancelling = true; cancelledAt = GetTickCount64(); lock.unlock(); send({1, encodeMessage({{"op", "cancel"}, {"id", std::to_string(nextId_++)}, {"target", std::to_string(id)}})}); lock.lock(); }
            if ((!search && GetTickCount64() > deadline) || (cancelling && GetTickCount64() - cancelledAt > 2000)) { lock.unlock(); fail(L"WSL operation timed out. Reconnect and verify the file before retrying a save or rename."); lock.lock(); break; }
        }
        if (!pending->done) throw std::runtime_error("WSL connection closed; operation outcome may be unknown");
        responseBytes_ -= pending->entryBytes + pending->response.bytes.size(); auto response = std::move(pending->response); pending_.erase(id); lock.unlock();
        const auto error = field(response.metadata, "error"); if (!error.empty()) throw std::runtime_error(error);
        if (cancelling) throw std::runtime_error(searchTimedOut ? "WSL content search timed out; narrow the search and retry." : "WSL request cancelled");
        return response;
    } catch (...) { std::lock_guard<std::mutex> lock(mutex_); const auto found = pending_.find(id); if (found != pending_.end()) { responseBytes_ -= found->second->entryBytes + found->second->response.bytes.size(); pending_.erase(found); } throw; }
}
std::vector<std::wstring> Connection::terminal(const ResourceId& path, const std::string& token, const std::wstring& tool) const {
    std::lock_guard<std::mutex> lock(mutex_); if (state_ != ConnectionState::Ready) throw std::runtime_error("Reconnect the WSL project before starting a terminal");
    auto args = command(); args.insert(args.end(), {wide(helperPath_), L"--terminal", wide(path.u8string()), wide(token)});
    if (tool.empty()) args.insert(args.end(), {wide(shell_), L"-l"});
    else {
        if (tool != L"codex" && tool != L"claude") throw std::runtime_error("Unsupported AI container tool");
        // aicontainer is a Bash function installed in the user's shell startup.
        args.insert(args.end(), {L"/bin/bash", L"-lic", L"aicontainer " + tool});
    }
    return args;
}
std::vector<std::wstring> Connection::terminalEvents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ConnectionState::Ready) throw std::runtime_error("Reconnect the WSL project before starting notifications");
    auto args = command(); args.insert(args.end(), {wide(helperPath_), L"--events"}); return args;
}
bool Connection::stopTerminal(const std::string& token) const {
    std::vector<std::wstring> args;
    { std::lock_guard<std::mutex> lock(mutex_); if (helperPath_.empty() || helperPath_[0] != '/') return false; args = command(); args.insert(args.end(), {wide(helperPath_), L"--stop-terminal", wide(token)}); }
    return runProcess(args, windowsProcessDirectory(), 4000).code == 0;
}
class WslProvider final : public ProjectProvider {
public:
    WslProvider(const ResourceId& root, std::shared_ptr<Connection> connection) : connection_(std::move(connection)), project_(sha256(root.key()).substr(0, 32)) {}
    ~WslProvider() override { std::lock_guard<std::mutex> lock(connection_->mutex_); connection_->projectWatches_.erase(project_); }
    ResourceId connect(const ResourceId& root) override {
        projectState_ = ConnectionState::Connecting;
        try {
            connection_->ensure();
            if (!expectedUid_.empty() && identity() != expectedUid_) throw std::runtime_error("The WSL user UID changed. Reopen this project with the previous Linux user; saved edits have been kept.");
            const auto response = call("connect", root);
            const auto resolved = ResourceId::wsl(root.location().distro, field(response.metadata, "path"), root.location().user);
            { std::lock_guard<std::mutex> lock(connection_->mutex_); auto& watch = connection_->projectWatches_[project_]; watch.root = resolved; watch.changes = {}; watch.changes.watchLimited = connection_->watchLimited_; }
            projectState_ = ConnectionState::Ready; return resolved;
        }
        catch (...) { projectState_ = ConnectionState::Unavailable; throw; }
    }
    GitRepositoryScan gitRepositories(const ResourceId& root, const std::function<bool()>& cancelled) override {
        GitRepositoryScan result;
        try {
            const auto response = call("gitRepositories", root, {}, nullptr, cancelled, 60000);
            result.truncated = field(response.metadata, "truncated") == "1";
            result.error = field(response.metadata, "scanError");
            for (const auto& entry : response.entries) result.roots.push_back(root.at(field(entry, "path")));
        } catch (const std::exception& error) { result.error = error.what(); }
        return result;
    }
    ProviderListing listDirectory(const ResourceId& path) override {
        ProviderListing result;
        try { const auto response = call("listDirectory", path); result.truncated = field(response.metadata, "truncated") == "1"; for (const auto& item : response.entries) { const bool unsupported = field(item, "unsupported") == "1"; result.entries.push_back({unsupported ? ResourceId{} : path.at(field(item, "path")), field(item, "directory") == "1", field(item, "link") == "1", unsupported, wide(field(item, "name"))}); } } catch (const std::exception& error) { result.error = wide(error.what()); }
        return result;
    }
    ProviderFile readFile(const ResourceId& path) override { try { auto response = call("readFile", path); return {std::move(response.bytes), field(response.metadata, "version"), {}}; } catch (const std::exception& error) { return {{}, {}, wide(error.what())}; } }
    ProviderSave saveFile(const ResourceId& path, const std::string& bytes, const std::string& version, const std::string&) override {
        try { if (bytes.size() > maxFileBytes || version.empty()) throw std::runtime_error("File cannot be saved without a valid version and a size below 16 MiB"); const auto response = call("saveFile", path, {{"size", std::to_string(bytes.size())}, {"version", version}}, &bytes); return {field(response.metadata, "version"), {}}; } catch (const std::exception& error) { return {{}, wide(error.what())}; }
    }
    std::wstring createFile(const ResourceId& path, const std::string& bytes) override {
        try {
            if (bytes.size() > maxFileBytes) throw std::runtime_error("New file exceeds the 16 MiB limit.");
            call("createFile", path, {{"size", std::to_string(bytes.size())}}, &bytes); return {};
        } catch (const std::exception& error) { return wide(error.what()); }
    }
    std::wstring createDirectory(const ResourceId& path, bool allowExisting) override {
        try { call("createDirectory", path, {{"allowExisting", allowExisting ? "1" : "0"}}); return {}; }
        catch (const std::exception& error) { return wide(error.what()); }
    }
    std::wstring renameResource(const ResourceId& source, const ResourceId& destination) override {
        try {
            if (!source.sameAuthority(destination)) return L"Cannot move between WSL connections.";
            const auto info = call("stat", source);
            call("renameResource", source, {{"name", destination.filename().u8string()}, {"parent", destination.parent_path().u8string()}, {"version", field(info.metadata, "version")}});
            return {};
        } catch (const std::exception& error) { return wide(error.what()); }
    }
    FileIndex indexFiles(const ResourceId& root, const std::function<bool()>& cancelled) override {
        struct Delivery { std::mutex mutex; FileIndex index; bool limited = false, finished = false; };
        auto delivery = std::make_shared<Delivery>();
        std::string error;
        try {
            // Convert each entry immediately. Retaining every JSON map before
            // building the index multiplied memory and exhausted the shared
            // connection receive budget, disconnecting unrelated projects.
            const auto response = call("indexFiles", root, {}, nullptr, cancelled, 60000,
                [delivery, root](const Message& entry) {
                    std::lock_guard<std::mutex> lock(delivery->mutex);
                    if (delivery->finished || delivery->limited) return;
                    const auto path = root.at(field(entry, "path"));
                    if (!root.contains(path) || root == path) throw std::runtime_error("Invalid WSL indexed path");
                    delivery->limited = !addIndexedFile(delivery->index, path.lexically_relative(root).u8string());
                });
            if (field(response.metadata, "truncated") == "1") error = "WSL file list reached its 200,000-file or 50,000-directory limit.";
        } catch (const std::exception& failure) { error = failure.what(); }
        // A failed connection may wake call() while its reader is delivering
        // an entry. Freeze the collector before returning the snapshot.
        std::lock_guard<std::mutex> lock(delivery->mutex);
        delivery->finished = true;
        auto index = std::move(delivery->index);
        if (index.error.empty()) index.error = std::move(error);
        std::sort(index.files.begin(), index.files.end(), [](const IndexedFile& a, const IndexedFile& b) { return a.relative < b.relative; });
        return index;
    }
    ContentSearchSummary searchContent(const ResourceId& root, const ContentSearchOptions& options,
        const ContentPublish& publish, const std::function<bool()>& cancelled) override {
        struct Delivery { ContentPublish publish; std::atomic<bool> full{false}; size_t count = 0, bytes = 0; };
        auto delivery = std::make_shared<Delivery>(); delivery->publish = publish;
        auto message = contentSearchMessage(options); std::string exclusions;
        for (const auto& relative : options.excluded) {
            if (!validSearchRelative(relative) || exclusions.size() + relative.size() + 1 > 4 * 1024 * 1024) throw std::runtime_error("Invalid or oversized dirty-buffer exclusions");
            exclusions += relative; exclusions += '\0';
        }
        message["size"] = std::to_string(exclusions.size());
        try {
            const auto response = call("searchContent", root, std::move(message), &exclusions,
                [cancelled, delivery] { return delivery->full.load() || (cancelled && cancelled()); }, 600000,
                [delivery](const Message& entry) {
                    if (delivery->full) return;
                    auto match = contentMatchFromMessage(entry); delivery->bytes += contentMatchBytes(match);
                    if (++delivery->count > contentResultLimit || delivery->bytes > contentResultBytes) throw std::runtime_error("WSL search exceeded its result budget");
                    if (delivery->publish && !delivery->publish({std::move(match)})) delivery->full = true;
                });
            return contentSummaryFromMessage(response.metadata);
        } catch (...) { if (delivery->full) { ContentSearchSummary summary; summary.limited = true; return summary; } throw; }
    }
    ProcessResult git(const std::vector<std::wstring>& args, const ResourceId& directory, const std::atomic<bool>* cancelled) override {
        try { Message extra{{"argc", std::to_string(args.size() - 1)}}; for (size_t i = 1; i < args.size(); ++i) extra["arg" + std::to_string(i - 1)] = utf8(args[i]); auto response = call("git", directory, std::move(extra), nullptr, [cancelled] { return cancelled && *cancelled; }, 20000); ProcessResult result; result.code = static_cast<DWORD>(number(response.metadata, "exitCode")); result.output = std::move(response.bytes); if (result.code) result.error = result.code == 127 ? L"Git is not installed in this WSL distribution. Install Git there and press F5." : validUtf8(result.output) ? wide(result.output) : L"Linux Git failed"; return result; } catch (const std::exception& error) { return {1, {}, wide(error.what())}; }
    }
    ConnectionState state() const override { return connection_->state() == ConnectionState::Ready ? projectState_.load() : connection_->state(); }
    std::wstring error() const override { return connection_->error(); }
    uint64_t revision() const override { return connection_->revision(); }
    ProviderChanges takeChanges() override {
        std::lock_guard<std::mutex> lock(connection_->mutex_);
        const auto found = connection_->projectWatches_.find(project_);
        return found == connection_->projectWatches_.end() ? ProviderChanges{} : std::exchange(found->second.changes, ProviderChanges{});
    }
    void reconnect() override { connection_->reconnect(); ++generation_; }
    void expectIdentity(std::string uid) override { expectedUid_ = std::move(uid); }
    std::string identity() const override { std::lock_guard<std::mutex> lock(connection_->mutex_); return connection_->uid_; }
    std::vector<std::wstring> terminalCommand(const ResourceId& path, const std::string& token, const std::wstring& tool) const override { if (state() != ConnectionState::Ready) throw std::runtime_error("Reconnect the WSL project before starting a terminal"); return connection_->terminal(path, token, tool); }
    std::vector<std::wstring> terminalEventsCommand() const override { return connection_->terminalEvents(); }
    bool stopTerminal(const std::string& token) const override { return connection_->stopTerminal(token); }
private:
    Response call(const std::string& op, const ResourceId& path, Message extra = {}, const std::string* bytes = nullptr, const std::function<bool()>& cancelled = {}, DWORD timeout = 10000,
        std::function<void(const Message&)> receive = {}) {
        if (connection_->state() != ConnectionState::Ready || (op != "connect" && projectState_ != ConnectionState::Ready)) throw std::runtime_error("WSL is disconnected. Use Reconnect or F5 to retry. " + utf8(connection_->error()));
        extra.insert({{"op", op}, {"path", path.u8string()}, {"projectId", project_}, {"generation", std::to_string(generation_.load())}}); return connection_->call(std::move(extra), bytes, cancelled, timeout, std::move(receive));
    }
    std::shared_ptr<Connection> connection_;
    std::string project_;
    std::string expectedUid_;
    std::atomic<uint64_t> generation_{1};
    std::atomic<ConnectionState> projectState_{ConnectionState::Disconnected};
};
}
WslEventLaunch defaultWslTerminalEvents() {
    WslEventLaunch launch; launch.input = bundledWslHelper();
    launch.command = {wslExecutable(), L"--exec", L"/bin/sh", L"-c", wide(wsl::bootstrap),
        L"poppy-bootstrap", wide(wsl::helperVersion), wide(wsl::sha256(launch.input)), std::to_wstring(launch.input.size()), L"--events"};
    return launch;
}
std::shared_ptr<ProjectProvider> createWslProvider(const ResourceId& root) {
    static std::mutex mutex; static std::map<std::string, std::weak_ptr<Connection>> connections;
    std::lock_guard<std::mutex> lock(mutex); for (auto it = connections.begin(); it != connections.end();) { if (it->second.expired()) it = connections.erase(it); else ++it; } const auto key = root.at("/").key(); auto connection = connections[key].lock(); if (!connection) { connection = std::make_shared<Connection>(root.location()); connections[key] = connection; }
    return std::make_shared<WslProvider>(root, std::move(connection));
}
}
