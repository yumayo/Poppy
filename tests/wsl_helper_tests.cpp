#include "wsl_protocol.h"
#include "git_history.h"
#include "content_search_protocol.h"
#include "wsl_bootstrap.h"
#include "text.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace poppy::wsl;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    fs::path root;
    Fixture() { std::string name = (fs::current_path()/"Poppy WSL 日本語 ' $ XXXXXX").string(); const auto created = mkdtemp(name.data()); check(created != nullptr,"Cannot create helper fixture in the build directory"); root = created; }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::system_error(errno, std::generic_category(), "Cannot open fixture: " + path.string());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); file.close();
    if (!file) throw std::system_error(errno ? errno : EIO, std::generic_category(), "Cannot write fixture: " + path.string());
}
std::string disk(const fs::path& path) { std::ifstream file(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(file), {}); }
struct Result { Message metadata; std::vector<Message> entries; std::string bytes; bool last = false; };
class Helper {
public:
    explicit Helper(const std::string& executable, const fs::path& bootstrapHome = {}) {
        const std::string binary = bootstrapHome.empty() ? "" : disk(executable), hash = sha256(binary), size = std::to_string(binary.size());
        int toChild[2], fromChild[2]; check(!pipe(toChild) && !pipe(fromChild),"Cannot create test pipes");
        child_ = fork(); check(child_ >= 0,"Cannot start test helper");
        if (!child_) { dup2(toChild[0],0); dup2(fromChild[1],1); close(toChild[0]); close(toChild[1]); close(fromChild[0]); close(fromChild[1]); if (!bootstrapHome.empty()) { setenv("HOME",bootstrapHome.c_str(),1); execl("/bin/sh","/bin/sh","-c",bootstrap,"poppy-bootstrap",helperVersion,hash.c_str(),size.c_str(),nullptr); } else execl(executable.c_str(),executable.c_str(),"--stdio",nullptr); _exit(127); }
        input_ = toChild[1]; output_ = fromChild[0]; close(toChild[0]); close(fromChild[1]);
        if (!binary.empty()) raw(binary);
    }
    ~Helper() { if (input_ >= 0) close(input_); if (output_ >= 0) close(output_); if (child_ > 0) { int status; if (!waitpid(child_, &status, WNOHANG)) { kill(child_, SIGKILL); waitpid(child_,&status,0); } } }
    void frame(const Frame& frame) { raw(encodeFrame(frame)); }
    void raw(const std::string& bytes) { size_t at = 0; while (at < bytes.size()) { const auto count = ::write(input_,bytes.data()+at,bytes.size()-at); check(count > 0,"Helper input disconnected"); at += static_cast<size_t>(count); } }
    uint64_t request(Message m, const std::string* bytes = nullptr) {
        const auto id = next_++; m["id"] = std::to_string(id); m["projectId"] = "fixture"; m["generation"] = "7"; frame({1,encodeMessage(m)});
        if (bytes) { size_t at = 0; do { const size_t count = std::min(maxChunk, bytes->size()-at); frame(dataFrame(id,at,at+count == bytes->size(),std::string_view(*bytes).substr(at,count))); at += count; } while (at < bytes->size()); }
        return id;
    }
    Result wait(uint64_t id, bool success = true) {
        Result result; const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(12);
        for (;;) {
            Frame frame;
            const auto pending = std::find_if(deferred_.begin(), deferred_.end(), [id](const Frame& f) { return f.kind == 1 ? number(decodeMessage(f.payload),"id") == id : decodeChunk(f).id == id; });
            if (pending != deferred_.end()) { frame = std::move(*pending); deferred_.erase(pending); } else frame = next(deadline);
            if (frame.kind == 1) {
                auto message = decodeMessage(frame.payload); if (!field(message,"event").empty()) { ++events_; continue; }
                if (number(message,"id") != id) { deferred_.push_back(std::move(frame)); continue; }
                check(field(message,"projectId") == "fixture" && field(message,"generation") == "7","Helper response crossed projects/generations");
                if (field(message,"entry") == "1") result.entries.push_back(std::move(message));
                else { for (const auto& item : message) result.metadata[item.first] = item.second; if (field(message,"done") == "1") { if (success) check(field(message,"error").empty(),field(message,"error").c_str()); else check(!field(message,"error").empty(),"Expected operation to fail"); return result; } }
            } else {
                const auto chunk = decodeChunk(frame); if (chunk.id != id) { deferred_.push_back(std::move(frame)); continue; }
                check(!result.last && chunk.offset == result.bytes.size(),"Body chunks are out of order"); result.bytes += chunk.bytes; result.last = chunk.last;
            }
        }
    }
    Result call(const std::string& op, const fs::path& path, Message extra = {}, const std::string* bytes = nullptr, bool success = true) { extra["op"] = op; extra["path"] = path.string(); return wait(request(std::move(extra),bytes), success); }
    void hello() { check(field(wait(request({{"op","hello"},{"protocolVersion","1"}})).metadata,"architecture") == "x86_64","Unexpected helper architecture"); }
    bool event() {
        if (events_) return true;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) { auto f = next(deadline); if (f.kind == 1 && field(decodeMessage(f.payload),"event") == "rescanRequired") return true; }
        return false;
    }
    void eof() {
        close(input_); input_ = -1; const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3); int status = 0;
        while (waitpid(child_,&status,WNOHANG) == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(waitpid(child_,&status,WNOHANG) == -1,"Helper did not shut down after EOF"); child_ = -1;
    }
private:
    Frame next(std::chrono::steady_clock::time_point deadline) {
        // Defer responses for other requests until the current request completes.
        if (!frames_.empty()) { auto frame = std::move(frames_.front()); frames_.pop_front(); return frame; }
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Helper response timed out");
        pollfd fd{output_,POLLIN,0}; check(poll(&fd,1,1000) >= 0,"Cannot poll helper");
        if (fd.revents & (POLLIN|POLLHUP)) { char bytes[65536]; const auto count = ::read(output_,bytes,sizeof(bytes)); check(count > 0,"Helper output disconnected"); reader_.feed(std::string_view(bytes,static_cast<size_t>(count)),[&](Frame f){ frames_.push_back(std::move(f)); }); }
        return next(deadline);
    }
    pid_t child_ = -1; int input_ = -1, output_ = -1; uint64_t next_ = 1;
    FrameReader reader_; std::deque<Frame> frames_, deferred_; size_t events_ = 0;
};
std::string version(const Result& result) { return field(result.metadata,"version"); }
void creation(const std::string& executable) {
    Fixture fixture; Helper helper(executable); helper.hello(); helper.call("connect", fixture.root);
    const auto folder = fixture.root / "新規 フォルダー", file = folder / "空のファイル.txt";
    const std::string empty, replacement = "must not overwrite";
    helper.call("createDirectory", folder);
    check(fs::is_directory(folder) && fs::is_empty(folder), "New folder was not empty or used the wrong path");
    helper.call("createFile", file, {{"size", "0"}}, &empty);
    check(fs::is_regular_file(file) && fs::file_size(file) == 0, "New text file was not empty");
    write(file, "keep existing content");
    helper.call("createFile", file, {{"size", std::to_string(replacement.size())}}, &replacement, false);
    helper.call("createDirectory", file, {}, nullptr, false);
    helper.call("createDirectory", folder, {}, nullptr, false);
    helper.call("createFile", folder, {{"size", "0"}}, &empty, false);
    check(disk(file) == "keep existing content", "File/folder collision overwrote existing content");
    const auto link = fixture.root / "folder-link"; fs::create_directory_symlink(folder, link);
    helper.call("createDirectory", link, {}, nullptr, false);
    check(fs::is_symlink(link), "Folder creation replaced an existing link");
    const auto dangling = fixture.root / "dangling"; fs::create_symlink(fixture.root / "absent", dangling);
    helper.call("createDirectory", dangling, {}, nullptr, false);
    check(fs::is_symlink(dangling) && !fs::exists(fixture.root / "absent"), "Folder creation followed a dangling link");
    helper.call("createDirectory", fixture.root / "missing" / "child", {}, nullptr, false);
    helper.call("createFile", fixture.root / "missing" / "child.txt", {{"size", "0"}}, &empty, false);
    check(!fs::exists(fixture.root / "missing"), "Creation unexpectedly made intermediate folders");
    helper.call("createDirectory", fixture.root / "CON");
    helper.call("createDirectory", fixture.root / "con");
    check(fs::is_directory(fixture.root / "CON") && fs::is_directory(fixture.root / "con"), "Folder creation lost Linux name semantics");
    // Nested creation reuses real intermediate directories, while the final
    // component retains the existing no-overwrite behavior.
    const Message intermediate{{"allowExisting", "1"}};
    helper.call("createDirectory", folder, intermediate);
    const auto nested = folder / "path" / "to";
    helper.call("createDirectory", nested.parent_path(), intermediate);
    helper.call("createDirectory", nested, intermediate);
    helper.call("createFile", nested / "file.md", {{"size", "0"}}, &empty);
    check(fs::is_regular_file(nested / "file.md") && fs::file_size(nested / "file.md") == 0, "Nested file creation failed");
    helper.call("createDirectory", nested, intermediate);
    helper.call("createDirectory", nested / "folder");
    check(fs::is_directory(nested / "folder"), "Nested folder creation failed");
    helper.call("createDirectory", file, intermediate, nullptr, false);
    helper.call("createDirectory", link, intermediate, nullptr, false);
    helper.call("createDirectory", dangling, intermediate, nullptr, false);
    check(disk(file) == "keep existing content" && fs::is_symlink(link) && fs::is_symlink(dangling), "Intermediate folder handling changed an existing item");
    fs::remove_all(folder / "path");
    for (const auto& entry : fs::directory_iterator(folder)) check(entry.path() == file, "Failed creation left a temporary file");
    helper.eof();
}
void files(const std::string& executable) {
    Fixture fixture; const auto a = fixture.root/"A.cpp", b = fixture.root/"a.cpp", unusual = fixture.root/"quote' $ \\ file.cpp";
    write(a,"upper"); write(b,"lower"); write(unusual,"日本語\r\n"); write(fixture.root/"empty","");
    fs::create_directory(fixture.root/"src"); fs::create_directory_symlink(fixture.root/"src",fixture.root/"directory-link");
    write(fixture.root/fs::path(std::string("invalid-\xff",9)),"bytes");
    Helper helper(executable); helper.hello();
    check(field(helper.call("connect",fixture.root).metadata,"path") == fixture.root.string(),"Project connection changed path");
    const auto listed = helper.call("listDirectory",fixture.root); std::set<std::string> names; bool unsupported = false;
    for (const auto& entry : listed.entries) { names.insert(field(entry,"name")); unsupported = unsupported || field(entry,"unsupported") == "1"; }
    check(names.count("A.cpp") && names.count("a.cpp") && names.count(unusual.filename().string()) && unsupported,"Linux listing lost names or encoding diagnostics");
    const auto indexed = helper.call("indexFiles",fixture.root); for (const auto& item : indexed.entries) check(field(item,"path").find("directory-link/") == std::string::npos,"Index followed a directory link");
    const auto image = fixture.root/"src"/"paste-2026-10-27T08-03-09.png";
    const std::string imageBytes("\x89PNG\r\n\x1a\n\0binary", 15), otherImage = "replacement";
    helper.call("createFile",image,{{"size",std::to_string(imageBytes.size())}},&imageBytes);
    check(disk(image) == imageBytes,"New image bytes were changed or saved in the wrong folder");
    helper.call("createFile",image,{{"size",std::to_string(otherImage.size())}},&otherImage,false);
    check(disk(image) == imageBytes,"Image collision overwrote an existing file");
    const auto imageLink = fixture.root/"image-link.png"; fs::create_symlink(image,imageLink);
    helper.call("createFile",imageLink,{{"size",std::to_string(otherImage.size())}},&otherImage,false);
    check(fs::is_symlink(imageLink) && disk(image) == imageBytes,"Image creation followed or replaced an existing symlink");
    helper.call("createFile",fixture.root/"src",{{"size",std::to_string(imageBytes.size())}},&imageBytes,false);
    helper.call("createFile",fixture.root/"missing"/"image.png",{{"size",std::to_string(imageBytes.size())}},&imageBytes,false);
    for (const auto& entry : fs::directory_iterator(image.parent_path())) check(entry.path().filename().string().find(".poppy-paste-") != 0,"Image creation leaked a temporary file");
    check(helper.call("readFile",unusual).bytes == disk(unusual),"UTF-8 bytes changed");
    check(helper.call("readFile",fixture.root/"empty").last,"Empty file omitted final chunk");
    const std::string utf16("\xff\xfe\x41\0\r\0\n\0",8); write(fixture.root/"utf16",utf16); check(helper.call("readFile",fixture.root/"utf16").bytes == utf16,"UTF-16 bytes changed");
    const std::string maximum(poppy::maxFileBytes,'x'); write(fixture.root/"maximum",maximum); check(helper.call("readFile",fixture.root/"maximum").bytes == maximum,"Maximum file did not round-trip");
    write(fixture.root/"too-large",maximum+'x'); helper.call("readFile",fixture.root/"too-large",{},nullptr,false);
    check(!mkfifo((fixture.root/"fifo").c_str(),0600),"Cannot create FIFO"); helper.call("readFile",fixture.root/"fifo",{},nullptr,false);
    auto baseline = helper.call("readFile",a); const std::string changed = "edited\r\n"; const auto oldVersion = version(baseline);
    chmod(a.c_str(),0751); baseline = helper.call("readFile",a);
    auto saved = helper.call("saveFile",a,{{"version",version(baseline)},{"size",std::to_string(changed.size())}},&changed);
    check(disk(a) == changed && !version(saved).empty(),"Atomic save failed"); struct stat mode{}; stat(a.c_str(),&mode); check((mode.st_mode & 0777) == 0751,"Save lost executable permissions");
    helper.call("saveFile",a,{{"version",oldVersion},{"size",std::to_string(changed.size())}},&changed,false); check(disk(a) == changed,"Conflict overwrote the file");
    baseline = helper.call("readFile",b); write(b,"externally changed"); helper.call("saveFile",b,{{"version",version(baseline)},{"size",std::to_string(changed.size())}},&changed,false); check(disk(b) == "externally changed","External edit was overwritten");
    const auto symlink = fixture.root/"file-link"; fs::create_symlink(a.filename(),symlink); baseline = helper.call("readFile",symlink);
    const std::string linkText = "saved through link"; helper.call("saveFile",symlink,{{"version",version(baseline)},{"size",std::to_string(linkText.size())}},&linkText); check(fs::is_symlink(symlink) && disk(a) == linkText,"Saving replaced a symbolic link");
    baseline = helper.call("readFile",symlink); fs::remove(symlink); fs::create_symlink(b.filename(),symlink); helper.call("saveFile",symlink,{{"version",version(baseline)},{"size",std::to_string(changed.size())}},&changed,false);
    fs::create_hard_link(a,fixture.root/"hard-link"); baseline = helper.call("readFile",a); helper.call("saveFile",a,{{"version",version(baseline)},{"size",std::to_string(changed.size())}},&changed,false);
    fs::remove(fixture.root/"hard-link");
    if (!setxattr(a.c_str(),"user.poppy-test","x",1,0)) { baseline = helper.call("readFile",a); helper.call("saveFile",a,{{"version",version(baseline)},{"size",std::to_string(changed.size())}},&changed,false); removexattr(a.c_str(),"user.poppy-test"); }
    auto info = helper.call("stat",a); helper.call("renameResource",a,{{"name","a.cpp"},{"version",version(info)}},nullptr,false); check(fs::exists(a) && fs::exists(b),"Rename overwrote another file");
    helper.call("renameResource",a,{{"name","CON"},{"version",version(info)}}); check(fs::exists(fixture.root/"CON"),"Linux reserved-name rename failed");
    info = helper.call("stat",b); helper.call("renameResource",b,{{"name","../escape"},{"version",version(info)}},nullptr,false);
    write(fixture.root/"notify","external update"); check(helper.event(),"inotify did not request a refresh");
    helper.eof();
}
void repositories(const std::string& executable) {
    Fixture fixture;
    const auto app = fixture.root / "app", client = app / "app-client", server = app / "app-server";
    fs::create_directories(client); fs::create_directories(server);
    Helper helper(executable); helper.hello(); helper.call("connect", app);
    auto git = [&](const fs::path& directory, std::vector<std::string> args) {
        Message message{{"argc", std::to_string(args.size())}};
        for (size_t i = 0; i < args.size(); ++i) message["arg" + std::to_string(i)] = args[i];
        auto result = helper.call("git", directory, std::move(message));
        check(number(result.metadata, "exitCode") == 0, result.bytes.c_str()); return result.bytes;
    };
    git(fixture.root, {"init", "--quiet", "--template=", "--initial-branch=main"});
    write(fixture.root / "unrelated.txt", "outside the opened folder\n");
    check(helper.call("gitRepositories", app).entries.empty(), "WSL discovery included an ancestor repository");
    for (const auto& directory : {client, server}) {
        git(directory, {"init", "--quiet", "--template=", "--initial-branch=main"});
        git(directory, {"config", "user.name", "Poppy Test"}); git(directory, {"config", "user.email", "poppy@example.invalid"});
        git(directory, {"config", "commit.gpgsign", "false"});
        write(directory / "same.txt", directory.filename().string() + "\n");
        git(directory, {"add", "."}); git(directory, {"commit", "-qm", directory.filename().string()});
    }
    auto scan = helper.call("gitRepositories", app);
    std::set<std::string> roots;
    for (const auto& entry : scan.entries) roots.insert(field(entry, "path"));
    check(roots == std::set<std::string>{client.string(), server.string()}, "WSL discovery missed siblings below a non-Git parent");
    write(client / "same.txt", "client edit\n"); write(server / "same.txt", "server edit\n");
    check(helper.event(), "A nested repository did not receive a watch");
    git(client, {"add", "same.txt"});
    check(git(client, {"status", "--porcelain=v1"}).find("M  same.txt") != std::string::npos &&
          git(server, {"status", "--porcelain=v1"}).find(" M same.txt") != std::string::npos, "WSL staging crossed repositories");
    check(git(server, {"log", "-1", "--format=%s"}) == "app-server\n", "WSL history used a sibling repository");
    const auto worktree = app / "worktree 日本語";
    git(client, {"worktree", "add", "--quiet", "--detach", worktree.string()});
    fs::create_directory_symlink(app, app / "loop");
    check(helper.call("gitRepositories", app).entries.size() == 3, "WSL discovery missed a gitfile or entered a directory link");
    fs::remove_all(server);
    check(helper.call("gitRepositories", app).entries.size() == 2, "WSL discovery retained a removed repository");
    helper.eof();
}
void protocolFailures(const std::string& executable) {
    { Helper helper(executable); helper.raw(std::string("\x7f\xff\xff\xff",4)); helper.eof(); }
    { Helper helper(executable); helper.raw(std::string("\0\0",2)); helper.eof(); }
    { Helper helper(executable); helper.hello(); helper.request({{"op","saveFile"},{"path","/tmp/unused"},{"size","1"},{"version","x"}}); helper.frame(dataFrame(2,1,true,"x")); helper.eof(); }
}
void installation(const std::string& executable) {
    Fixture fixture; const auto userHome = fixture.root/"home 日本語 ' $"; fs::create_directory(userHome);
    const auto hash = sha256(disk(executable)); const auto installed = userHome/".local/share/yumayo/Poppy/helpers"/(std::string(helperVersion)+'-'+hash)/"poppy-wsl-helper";
    { Helper helper(executable,userHome); helper.hello(); helper.eof(); }
    check(fs::exists(installed) && sha256(disk(installed)) == hash,"Bootstrap did not install the correct helper");
    const auto modified = fs::last_write_time(installed);
    {
        Helper helper(executable,userHome); helper.hello();
        check(fs::last_write_time(installed) == modified,"Bootstrap failed to reuse a verified helper");
        // Linux rejects truncating an executable that is still mapped (ETXTBSY).
        // Replace its directory entry instead, keeping this helper running to
        // cover a damaged installation while another process uses the old inode.
        const auto damaged = installed.parent_path()/"damaged-helper";
        write(damaged,"damaged helper"); fs::permissions(damaged,fs::perms::owner_all);
        fs::rename(damaged,installed);
        helper.eof();
    }
    { Helper helper(executable,userHome); helper.hello(); helper.eof(); }
    check(sha256(disk(installed)) == hash,"Bootstrap did not repair a checksum mismatch");
}
void gitAndCancellation(const std::string& executable) {
    Fixture fixture;
    {
        Helper helper(executable); helper.hello();
        auto git = [&](std::vector<std::string> args) { Message m{{"argc",std::to_string(args.size())}}; for (size_t i = 0; i < args.size(); ++i) m["arg"+std::to_string(i)] = args[i]; return helper.call("git",fixture.root,std::move(m)); };
        const auto available = git({"--version"});
        if (field(available.metadata,"exitCode") == "127") std::cout << "Git integration skipped: Git is not installed in the build container\n";
        else {
            check(field(available.metadata,"exitCode") == "0","Linux Git detection failed");
            check(field(git({"init","-q"}).metadata,"exitCode") == "0","Cannot initialize Linux Git fixture");
            write(fixture.root/"tracked 日本語.txt","before\n"); write(fixture.root/"delete.txt","delete\n"); write(fixture.root/"rename.txt","rename\n");
            git({"add","--","."}); check(field(git({"-c","user.name=Fixture","-c","user.email=fixture@example.invalid","commit","-qm","initial"}).metadata,"exitCode") == "0","Cannot commit Linux Git fixture");
            write(fixture.root/"tracked 日本語.txt","after\n"); fs::remove(fixture.root/"delete.txt"); git({"mv","rename.txt","renamed.txt"}); write(fixture.root/"untracked \\ '$.txt","untracked\n");
            const auto status = git({"--no-optional-locks","-c","core.fsmonitor=false","status","--porcelain=v1","-z","--untracked-files=all"});
            check(status.bytes.find("tracked 日本語.txt") != status.bytes.npos && status.bytes.find("delete.txt") != status.bytes.npos && status.bytes.find("renamed.txt") != status.bytes.npos && status.bytes.find("untracked \\ '$.txt") != status.bytes.npos && status.bytes.find('\0') != status.bytes.npos,"Linux Git lost porcelain NUL paths or change kinds");
            const auto diff = git({"diff","--no-ext-diff","--no-textconv","--patch","--","tracked 日本語.txt"}); check(diff.bytes.find("-before") != diff.bytes.npos && diff.bytes.find("+after") != diff.bytes.npos,"Linux Git patch was not returned");
            check(field(git({"add","--all","--"}).metadata,"exitCode") == "0","Cannot stage WSL changes for Unstage All Changes");
            write(fixture.root/"tracked 日本語.txt","partially staged\n");
            auto unstage = poppy::gitUnstageAllArguments(); unstage.erase(unstage.begin());
            check(field(git(std::move(unstage)).metadata,"exitCode") == "0" &&
                  field(git({"diff","--cached","--quiet"}).metadata,"exitCode") == "0" &&
                  disk(fixture.root/"tracked 日本語.txt") == "partially staged\n" && disk(fixture.root/"renamed.txt") == "rename\n" &&
                  disk(fixture.root/"untracked \\ '$.txt") == "untracked\n" && !fs::exists(fixture.root/"delete.txt") && !fs::exists(fixture.root/"rename.txt"),
                  "WSL Unstage All Changes left staged entries or changed working files");
            // Use the same discard plan through the helper, preserving its index.
            check(field(git({"add","--","tracked 日本語.txt"}).metadata,"exitCode") == "0", "Cannot partially stage WSL discard fixture");
            const auto indexBeforeDiscard = git({"ls-files","--stage","-z"}).bytes;
            write(fixture.root/"tracked 日本語.txt","discard this\n");
            git({"config","core.excludesFile","/dev/null"});
            write(fixture.root/".git/info/exclude","ignored.txt\n"); write(fixture.root/"ignored.txt","keep\n");
            const auto discardStatus = git({"status","--porcelain=v1","-z","--untracked-files=all"});
            for (auto command : poppy::gitDiscardAllArguments(poppy::parseGitStatus(discardStatus.bytes))) {
                command.erase(command.begin());
                check(field(git(std::move(command)).metadata,"exitCode") == "0", "WSL discard command failed");
            }
            check(git({"ls-files","--stage","-z"}).bytes == indexBeforeDiscard &&
                  disk(fixture.root/"tracked 日本語.txt") == "partially staged\n" && disk(fixture.root/"delete.txt") == "delete\n" &&
                  disk(fixture.root/"rename.txt") == "rename\n" && !fs::exists(fixture.root/"renamed.txt") &&
                  !fs::exists(fixture.root/"untracked \\ '$.txt") && disk(fixture.root/"ignored.txt") == "keep\n",
                  "WSL Discard All Changes lost staged content, retained changes or removed ignored files");
            git({"config","user.name","Fixture"}); git({"config","user.email","fixture@example.invalid"});
            write(fixture.root/"tracked 日本語.txt","stash working\n");
            write(fixture.root/"stash \\ '$.txt","stash untracked\n");
            auto stash = poppy::gitStashUnstagedArguments(poppy::parseGitStatus(git({"status","--porcelain=v1","-z","--untracked-files=all"}).bytes));
            stash.erase(stash.begin());
            check(field(git(std::move(stash)).metadata,"exitCode") == "0" &&
                  git({"ls-files","--stage","-z"}).bytes == indexBeforeDiscard &&
                  disk(fixture.root/"tracked 日本語.txt") == "partially staged\n" && !fs::exists(fixture.root/"stash \\ '$.txt") &&
                  disk(fixture.root/"ignored.txt") == "keep\n" &&
                  git({"show","stash@{0}:tracked 日本語.txt"}).bytes == "stash working\n" &&
                  git({"show","stash@{0}^3:stash \\ '$.txt"}).bytes == "stash untracked\n",
                  "WSL Stash Unstaged Changes lost staged content or failed to save working and untracked files");
        }
        helper.eof();
    }
    const auto bin = fixture.root/"bin"; fs::create_directory(bin);
    write(bin/"git","#!/bin/sh\nif [ \"$1\" = slow ]; then sleep 60 & echo $! > \"$2\"; wait; else shift; printf '%s\\0' \"$@\"; fi\n"); chmod((bin/"git").c_str(),0700);
    const auto inherited = getenv("PATH"); const std::string previous = inherited ? inherited : ""; setenv("PATH",(bin.string()+":/usr/bin:/bin").c_str(),1); Helper helper(executable); setenv("PATH",previous.c_str(),1); helper.hello();
    const std::string argument = "日本語 '\" $() \\ path";
    auto response = helper.call("git",fixture.root,{{"argc","2"},{"arg0","arguments"},{"arg1",argument}}); check(response.bytes == argument+'\0',"Git arguments were interpreted by a shell");
    const auto pidFile = fixture.root/"git-child"; write(fixture.root/"readable","responsive");
    const auto slow = helper.request({{"op","git"},{"path",fixture.root.string()},{"argc","2"},{"arg0","slow"},{"arg1",pidFile.string()}});
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3); while (!fs::exists(pidFile) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10)); check(fs::exists(pidFile),"Slow Git fixture did not start");
    check(helper.call("readFile",fixture.root/"readable").bytes == "responsive","Git blocked file/control requests");
    helper.request({{"op","cancel"},{"target",std::to_string(slow)}}); helper.wait(slow,false);
    const auto child = static_cast<pid_t>(std::stol(disk(pidFile))); check(kill(child,0) < 0,"Cancelled Git left a descendant"); helper.eof();
}
void contentSearch(const std::string& executable) {
    Fixture fixture;
    write(fixture.root/"A.cpp", "needle\r\nNEEDLE\n"); write(fixture.root/"a.cpp", u8"日本🙂 needle\n");
    const auto unusual = fixture.root / "back\\slash\nquote' $ file.cpp"; write(unusual, "needle");
    fs::create_directory(fixture.root/"ignored"); write(fixture.root/"ignored"/"no.cpp", "needle");
    write(fixture.root/".gitignore", "ignored/\n");
    write(fixture.root/"binary", std::string("needle\0",7));
    check(!mkfifo((fixture.root/"fifo").c_str(),0600), "Cannot create search FIFO");
    Helper helper(executable); helper.hello(); helper.call("connect",fixture.root);
    poppy::ContentSearchOptions options; options.query = "needle";
    const auto search = [&](std::string exclusions = "") {
        auto message = contentSearchMessage(options); message["size"] = std::to_string(exclusions.size());
        return helper.call("searchContent", fixture.root, message, &exclusions);
    };
    const auto result = search(); const auto summary = contentSummaryFromMessage(result.metadata);
    check(summary.matches == 4 && result.entries.size() == 4 && summary.binary == 1 && !summary.errors, "Helper content search omitted files, entered ignored folders, or opened a FIFO");
    bool exact = false;
    for (const auto& entry : result.entries) { const auto match = contentMatchFromMessage(entry); if (match.relative == unusual.filename().string()) exact = true; }
    check(exact, "Helper search changed a backslash/newline filename");
    std::string exclusions = "A.cpp"; exclusions += '\0'; const auto dirty = search(exclusions);
    check(contentSummaryFromMessage(dirty.metadata).matches == 2, "Helper searched an excluded dirty buffer");
    options.resultLimit = 1; const auto limited = search();
    check(limited.entries.size() == 1 && contentSummaryFromMessage(limited.metadata).limited, "Helper result limit was not reported");
    // Cancel before the final upload frame: deterministic cancellation without
    // timing a large filesystem fixture, while ordinary requests stay usable.
    auto message = contentSearchMessage(options); message["size"] = "0"; message["op"] = "searchContent"; message["path"] = fixture.root.string();
    const auto pending = helper.request(message);
    helper.request({{"op","cancel"},{"target",std::to_string(pending)}});
    check(helper.call("readFile",fixture.root/"A.cpp").bytes == "needle\r\nNEEDLE\n", "Search blocked file/control requests");
    helper.frame(dataFrame(pending,0,true,{}));
    check(field(helper.wait(pending, false).metadata, "code") == "cancelled", "Helper did not cancel a queued search");
    check(helper.call("readFile",unusual).bytes == "needle", "Cancelled search broke the shared connection"); helper.eof();
}
void terminals(const std::string& executable) {
    Fixture fixture;
    struct Session { pid_t pid; int master; };
    auto start = [&](const std::string& token,const std::string& name) {
        const auto master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        check(master >= 0 && !grantpt(master) && !unlockpt(master), "Cannot create terminal PTY");
        const std::string slave = ptsname(master);
        const auto child = fork(); check(child >= 0,"Cannot start terminal fixture");
        if (!child) { setsid(); const auto tty = open(slave.c_str(),O_RDWR); if (tty < 0 || ioctl(tty,TIOCSCTTY,0)) _exit(126); dup2(tty,0); dup2(tty,1); dup2(tty,2); close(tty); close(master); const auto file = fixture.root/name; const auto program = "sleep 60 & echo $! > \"$1\"; wait"; execl(executable.c_str(),executable.c_str(),"--terminal",fixture.root.c_str(),token.c_str(),"/bin/sh","-c",program,"fixture",file.c_str(),nullptr); _exit(127); }
        return Session{child,master};
    };
    const auto first = start("aaaaaaaa","one"), second = start("bbbbbbbb","two");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3); while ((!fs::exists(fixture.root/"one") || !fs::exists(fixture.root/"two")) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(fs::exists(fixture.root/"one") && fs::exists(fixture.root/"two"),"Terminal shell did not start in its Linux cwd");
    const auto one = static_cast<pid_t>(std::stol(disk(fixture.root/"one"))), two = static_cast<pid_t>(std::stol(disk(fixture.root/"two")));
    auto stop = [&](const char* token) { const auto child = fork(); if (!child) { execl(executable.c_str(),executable.c_str(),"--stop-terminal",token,nullptr); _exit(127); } int status; check(waitpid(child,&status,0) == child && WIFEXITED(status) && !WEXITSTATUS(status),"Terminal cleanup was not confirmed"); };
    stop("aaaaaaaa"); int status; waitpid(first.pid,&status,0); close(first.master); check(kill(one,0) < 0 && kill(two,0) == 0,"Stopping one terminal affected the other or left a child");
    stop("bbbbbbbb"); waitpid(second.pid,&status,0); close(second.master); check(kill(two,0) < 0,"Terminal descendant survived cleanup");
}
}
int main(int argc,char** argv) {
    signal(SIGPIPE,SIG_IGN);
    try { check(argc == 2,"Expected helper executable argument"); const std::string executable = argv[1]; installation(executable); creation(executable); files(executable); repositories(executable); protocolFailures(executable); gitAndCancellation(executable); contentSearch(executable); terminals(executable); std::cout << "WSL helper integration tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
