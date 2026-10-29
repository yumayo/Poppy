#include "resource.h"
#include "wsl_protocol.h"
#include "command_line.h"
#include "file_changes.h"
#include "limit_status.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void watchBudgets() {
    using namespace poppy;
    for (const uint64_t systemLimit : {uint64_t{0}, uint64_t{1}, uint64_t{2}, uint64_t{65536}, uint64_t{524288}, uint64_t{1048577}, UINT64_MAX}) {
        std::istringstream input(" \t" + std::to_string(systemLimit) + "\n");
        const auto usage = readWatchBudget(input);
        check(usage.systemLimit == systemLimit && usage.limit == systemLimit / 2 && usage.count == 0 && validWatchUsage(usage), "Watch budget was not half the startup Linux quota");
        auto full = usage; full.count = full.limit;
        check(validWatchUsage(full), "A full watch budget was rejected");
        ++full.count;
        check(!validWatchUsage(full), "A watch count above its budget was accepted");
    }
    for (const auto* value : {"", "invalid", "-1", "+10", "524288 trailing", "524288x", "18446744073709551616"}) {
        std::istringstream input(value);
        const auto usage = readWatchBudget(input);
        check(usage.limit == wsl::fallbackDirectoryWatches && usage.systemLimit == 0 && validWatchUsage(usage), "An unreadable Linux quota did not retain the fallback budget");
    }
    check(!validWatchUsage({0, 65536, 524288, false, {}}), "The old fixed budget was accepted with a larger Linux quota");
    check(!validWatchUsage({0, 262145, 524288, false, {}}), "A watch budget above half the Linux quota was accepted");
    check(!validWatchUsage({0, 1, 0, false, {}}), "An arbitrary fallback watch budget was accepted");
}
void limitStatuses() {
    using namespace poppy;
    check(groupedCount(0) == L"0" && groupedCount(1234) == L"1,234" && groupedCount(UINT64_MAX) == L"18,446,744,073,709,551,615", "Limit counters lost separators or integer precision");
    const auto disconnected = watchLimitStatus({});
    check(disconnected.text == L"Watch —/—" && !disconnected.warning, "An unknown watch budget was presented as a fixed limit");
    WatchUsage usage{1234, 262144, 524288, false, {}};
    auto status = watchLimitStatus(usage);
    check(status.text == L"Watch 1,234/262,144" && !status.warning && status.tooltip.find(L"524,288") != status.tooltip.npos && status.tooltip.find(L"Shared by all projects") != status.tooltip.npos && status.tooltip.find(L"half") != status.tooltip.npos && status.tooltip.find(L"startup") != status.tooltip.npos, "Watch tooltip omitted its scope or dynamic Linux budget");
    usage.limited = true; usage.reason = "systemLimit"; status = watchLimitStatus(usage);
    check(status.warning && status.tooltip.find(L"Linux could not allocate") != status.tooltip.npos && status.tooltip.find(L"F5") != status.tooltip.npos, "Partial monitoring below the Poppy cap was not explained");
    usage.count = usage.limit; usage.reason = "poppyLimit"; status = watchLimitStatus(usage);
    check(status.warning && status.tooltip.find(L"Poppy's directory watch limit") != status.tooltip.npos, "Watch capacity was not highlighted");
    std::istringstream unavailable;
    status = watchLimitStatus(readWatchBudget(unavailable));
    check(status.text == L"Watch 0/65,536" && status.tooltip.find(L"fallback") != status.tooltip.npos && !status.warning, "A fallback watch budget was not explained");
    std::istringstream zero("0");
    status = watchLimitStatus(readWatchBudget(zero));
    check(status.text == L"Watch 0/0" && status.warning && status.tooltip.find(L"Linux user watch limit: 0") != status.tooltip.npos, "A zero Linux watch quota was presented as unknown");
    FileIndex index; index.files.resize(1234); index.storageBytes = 3 * 1024 * 1024 / 2;
    status = fileLimitStatus(&index, true);
    check(status.text == L"Files 1,234/200,000 …" && status.tooltip.find(L"1.5/64 MiB") != status.tooltip.npos && status.tooltip.find(L"last completed scan") != status.tooltip.npos, "File status did not distinguish the cached scan and memory budget");
    index.error = "File list reached its file-count or memory limit.";
    check(fileLimitStatus(&index, false).warning && fileLimitStatus(&index, false).tooltip.find(L"partial list") != std::wstring::npos, "A partial file index was not highlighted");
    status = treeLimitStatus(50000, 50000, 10000);
    check(status.text == L"Tree 50,000/50,000" && status.warning && status.tooltip.find(L"cached children") != status.tooltip.npos, "Explorer status did not include cached items and its limit");
    status = treeLimitStatus(10001, 50000, 10000, 1);
    check(status.warning && status.text.find(L"!") != status.text.npos && status.tooltip.find(L"incomplete listings") != status.tooltip.npos, "Per-folder truncation below the total tree cap was hidden");
}
void changeClassification() {
    using poppy::classifyFileChange;
    for (const auto& path : {".git/index.lock", "nested/.git/HEAD.lock", "nested/.git/refs/heads/main.lock", "nested/.git/objects/ab/cd", "nested/.git/logs/HEAD", "nested/.git/FETCH_HEAD"})
        check(!classifyFileChange(path, true).relevant, "Git bookkeeping triggered a rescan");
    for (const auto& path : {".git/HEAD", "nested/.git/index", "nested/.git/config", "nested/.git/packed-refs", "nested/.git/refs/heads/main"}) {
        const auto change = classifyFileChange(path, true);
        check(change.relevant && !change.files && !change.content, "Git metadata invalidated file lists or documents");
    }
    const auto content = classifyFileChange("nested/file.txt", false), added = classifyFileChange("nested/file.txt", true);
    check(content.relevant && content.content && !content.files, "A content edit invalidated the file index");
    check(added.relevant && added.content && added.files, "An added file did not invalidate the index");
    check(classifyFileChange("nested/.git", true).files, "Adding a repository did not invalidate repository discovery");
    check(classifyFileChange("nested/.GIT/index.lock", true).files && classifyFileChange("nested/.git\\index.lock", true).files, "Linux filename semantics were lost");
    check(!classifyFileChange("nested\\.GIT\\index.lock", true, true).relevant, "Nested Windows Git locks triggered a rescan");
    const auto head = classifyFileChange("nested\\.GIT\\HEAD", true, true);
    check(head.relevant && !head.files && !head.content, "Windows HEAD change was ignored or reindexed files");
}
template<class F> void rejects(F operation) { try { operation(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid input was accepted"); }
void commandLines() {
    using poppy::processCommandLine;
    const std::vector<std::wstring> list{L"C:\\Windows\\System32\\wsl.exe", L"--list", L"--quiet"};
    check(processCommandLine(list) == L"C:\\Windows\\System32\\wsl.exe --list --quiet", "WSL list options must not be quoted or they become a Linux shell command");
    const std::vector<std::wstring> connect{L"wsl.exe", L"--distribution", L"Ubuntu", L"--user", L"user", L"--exec", L"/bin/sh", L"-c", L"printf '%s' \"$HOME\"\n"};
    check(processCommandLine(connect).rfind(L"wsl.exe --distribution Ubuntu --user user --exec /bin/sh -c ", 0) == 0, "WSL helper launch options must remain visible to wsl.exe");
    const std::vector<std::wstring> terminal{L"wsl.exe", L"--distribution", L"Ubuntu", L"--exec", L"/home/user/helper", L"--terminal", L"/home/user/日本語 ' $ project", L"token", L"/bin/bash", L"-l"};
    check(processCommandLine(terminal).rfind(L"wsl.exe --distribution Ubuntu --exec /home/user/helper --terminal ", 0) == 0, "WSL terminal launch lost its options");
    check(poppy::quoteArg(L"") == L"\"\"", "Empty arguments must be retained");
    check(poppy::quoteArg(L"C:\\path with spaces\\") == L"\"C:\\path with spaces\\\\\"", "A trailing backslash consumed its closing quote");
    check(poppy::quoteArg(L"a\"b") == L"\"a\\\"b\"", "An embedded quote was not escaped");
#ifdef _WIN32
    const auto checkArguments = [](const std::vector<std::wstring>& args) {
        const auto line = poppy::processCommandLine(args); int count = 0;
        auto decoded = CommandLineToArgvW(line.c_str(), &count);
        if (!decoded) throw std::runtime_error("Windows could not decode the command line");
        bool same = count == static_cast<int>(args.size());
        for (int i = 0; same && i < count; ++i) same = args[static_cast<size_t>(i)] == decoded[i];
        LocalFree(decoded); check(same, "Windows argument decoding changed a path, script or empty argument");
    };
    checkArguments(list); checkArguments(connect); checkArguments(terminal);
    checkArguments({L"Poppy.exe", L"--project", L"\\\\wsl.localhost\\Ubuntu-22.04\\home\\user\\project",
        L"--project=\\\\wsl$\\Ubuntu\\home\\user\\日本語 project"});
    checkArguments({L"C:\\Program Files\\Poppy\\Poppy.exe", L"", L"C:\\space path\\", L"/tmp/back\\slash", L"/tmp/a\"b", L"/tmp/a;b&$(pwd)'", L"line1\nline2"});
#endif
}
void resources() {
    using poppy::ResourceId;
    const auto root = ResourceId::wsl("Ubuntu", "/home/user/日本語 ' $ project");
    check(ResourceId::parse(root.serialized()).key() == root.key(), "WSL authority did not round-trip");
    check(root.at("/home/A").key() != root.at("/home/a").key(), "Linux case was folded");
    check(root.at("/home/é").key() != root.at("/home/é").key(), "Unicode normalization merged Linux files");
    check(root.at("/tmp/a\\b").filename().u8string() == "a\\b", "Linux backslash became a separator");
    check(root.at("/link/../file").lexically_normal().u8string() == "/link/../file", "Symlink traversal was simplified on Windows");
    check(ResourceId::wsl("Ubuntu", "/mnt/c/project").key() != ResourceId(std::filesystem::u8path("C:/project")).key(), "WSL and Windows merged");
    check(ResourceId::wsl("a:b", "/c", "d").key() != ResourceId::wsl("a", "/c", "b:d").key(), "Authority tuple is ambiguous");
    const auto source = root.at("/home/src"), destination = root.at("/home/new");
    check(poppy::relocateResource(root.at("/home/src/a\\b"), source, destination).u8string() == "/home/new/a\\b", "Rename lost a Linux filename");
    check(poppy::relocateResource(root.at("/home/src-old/a"), source, destination).u8string() == "/home/src-old/a", "Rename crossed a directory boundary");
    check(!ResourceId::wsl("Debian", "/home/src").contains(source), "Rename crossed distributions");
    check(poppy::validPosixName("CON") && poppy::validPosixName("x\\y: ") && !poppy::validPosixName("a/b") && !poppy::validPosixName(std::string("a\0b",3)), "Linux name rules changed");
    check(!poppy::validPosixName(std::string(256, 'x')) && !poppy::validUtf8("\xff"), "Filename limits ignored");
    for (const auto& unc : {"\\\\wsl.localhost\\Ubuntu\\home\\User", "\\\\WSL$\\Ubuntu\\home\\User"}) {
        const auto location = poppy::wslUncLocation(unc); check(location && location->distro == "Ubuntu" && location->root == "/home/User", "WSL UNC alias was not normalized");
    }
    check(!poppy::wslUncLocation("\\\\server\\Ubuntu\\home") && !poppy::wslUncLocation("\\\\wsl.localhost.other\\Ubuntu"), "SMB path became a WSL project");
    std::string utf16; for (const unsigned char c : std::string("Ubuntu\r\nDebian\n\0",16)) { utf16 += static_cast<char>(c); utf16 += '\0'; }
    check(poppy::parseWslDistributions(utf16) == std::vector<std::string>({"Ubuntu","Debian"}), "UTF-16 WSL list was misread");
    check(poppy::parseWslDistributions("Ubuntu\nDebian\r\n") == std::vector<std::string>({"Ubuntu","Debian"}), "UTF-8 WSL list was misread");
    rejects([] { ResourceId::parse("poppy-wsl:zz::2f"); });
    rejects([] { ResourceId::wsl("Ubuntu", "relative"); });
    rejects([] { poppy::parseWslDistributions(std::string("U\0x",3)); });
}
void protocol() {
    using namespace poppy::wsl;
    const Message message{{"id","31"},{"projectId","p-4"},{"generation","7"},{"path","/日本語/\"\n\\file"}};
    check(decodeMessage(encodeMessage(message)) == message, "JSON control round-trip failed");
    check(field(decodeMessage("{\"id\":31,\"text\":\"\\ud83c\\udf3a\"}"), "text") == "🌺", "JSON surrogate pair was misread");
    const auto frame = encodeFrame({1,encodeMessage(message)});
    for (size_t split = 0; split <= frame.size(); ++split) {
        FrameReader reader; int count = 0; auto receive = [&](Frame f) { check(f.kind == 1 && decodeMessage(f.payload) == message, "Split frame changed"); ++count; };
        reader.feed(std::string_view(frame).substr(0,split), receive); reader.feed(std::string_view(frame).substr(split), receive); reader.finish(); check(count == 1, "Split frame lost or duplicated");
    }
    FrameReader byteReader; int byteCount = 0; for (char c : frame + frame) byteReader.feed(std::string_view(&c,1), [&](Frame) { ++byteCount; }); byteReader.finish(); check(byteCount == 2,"Bytewise framing failed");
    const std::string bytes("\0\xff\r\n",4); const auto data = dataFrame(42,17,true,bytes); const auto chunk = decodeChunk(data); check(chunk.id == 42 && chunk.offset == 17 && chunk.last && chunk.bytes == bytes, "Binary body changed");
    rejects([] { FrameReader reader; reader.feed(std::string("\x7f\xff\xff\xff",4), [](Frame){}); });
    rejects([] { FrameReader reader; reader.feed(std::string("\0\0\0\0",4), [](Frame){}); });
    rejects([] { FrameReader reader; reader.feed(std::string("\0\0\0\x01\x03",5), [](Frame){}); });
    rejects([&] { FrameReader reader; reader.feed(std::string_view(frame).substr(0, frame.size()-1), [](Frame){}); reader.finish(); });
    rejects([] { decodeMessage("{\"id\":\"1\",\"id\":\"2\"}"); });
    rejects([] { decodeMessage("{\"path\":\"\\ud800\"}"); });
    rejects([] { decodeMessage("{\"path\":\"bad\nname\"}"); });
    rejects([] { number({{"id","18446744073709551616"}}, "id"); });
    rejects([] { encodeFrame({1,std::string(maxControl+1,'x')}); });
    rejects([] { decodeChunk({2,"short"}); });
    check(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty vector failed");
    check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc vector failed");
    check(sha256(std::string(1000000,'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 long vector failed");
}
void frameBatching() {
    using namespace poppy::wsl;
    std::vector<std::string> writes;
    FrameBatch batch([&](std::string_view bytes) { writes.emplace_back(bytes); });
    batch.flush(); check(writes.empty(), "An empty batch wrote to the pipe");
    const std::string name = u8"資料/日本🙂\"\\\n" + std::string(120, 'x');
    std::string expected;
    constexpr size_t count = 2000;
    for (size_t i = 0; i < count; ++i) {
        const Frame frame{1, encodeMessage({{"id", std::to_string(i)}, {"name", name}, {"entry", "1"}})};
        expected += encodeFrame(frame); batch.append(frame);
    }
    batch.flush();
    check(writes.size() > 1 && writes.size() < count / 10, "Small frames were not coalesced into bounded batches");
    std::string actual; size_t received = 0;
    for (const auto& bytes : writes) {
        check(bytes.size() <= maxOutputBatch, "A batch exceeded its encoded byte budget");
        actual += bytes; FrameReader reader;
        for (size_t at = 0; at < bytes.size(); at += 13) {
            reader.feed(std::string_view(bytes).substr(at, 13), [&](Frame frame) {
                const auto message = decodeMessage(frame.payload);
                check(number(message, "id") == received++ && field(message, "name") == name,
                    "Batched partial pipe reads changed frame order, Unicode or escaped names");
            });
        }
        reader.finish(); // Each output batch ends at a complete frame boundary.
    }
    check(actual == expected && received == count, "Batching lost, duplicated or changed frames");
    const auto sent = writes.size(); batch.flush();
    check(writes.size() == sent, "Flushing an already sent tail duplicated frames");
    std::cout << "WSL batching: " << count << " entry frames in " << sent << " output batches\n";
}
void frameBatchBoundaries() {
    using namespace poppy::wsl;
    auto control = [](size_t payloadBytes) {
        const auto overhead = encodeMessage({{"text", ""}}).size();
        return Frame{1, encodeMessage({{"text", std::string(payloadBytes - overhead, 'x')}})};
    };
    std::vector<std::string> writes;
    FrameBatch batch([&](std::string_view bytes) { writes.emplace_back(bytes); });
    batch.append(control(maxOutputBatch - 5));
    check(writes.size() == 1 && writes[0].size() == maxOutputBatch, "An exact-size batch was not flushed");
    const Frame small{1, encodeMessage({{"text", u8"日本語\"\\\n"}})};
    const auto bytes = encodeFrame(small);
    writes.clear();
    batch.append(small); batch.append(control(maxOutputBatch - bytes.size() - 5));
    check(writes.size() == 1 && writes[0].size() == maxOutputBatch,
        "Escaped JSON and framing bytes were not counted at the exact boundary");
    writes.clear();
    batch.append(small); batch.append(control(maxOutputBatch - bytes.size() - 4));
    check(writes.size() == 1 && writes[0] == bytes, "A frame was split instead of flushing the previous batch");
    batch.flush();
    check(writes.size() == 2 && writes[1].size() <= maxOutputBatch, "A partial final batch was lost");
    writes.clear();
    // The existing 64 KiB JSON limit excludes its five framing bytes.
    batch.append(small); batch.append(control(maxControl)); batch.flush();
    check(writes.size() == 2 && writes[0] == bytes && writes[1].size() == maxControl + 5,
        "A valid single frame larger than the batch target was split or rejected");
    writes.clear(); batch.append(small);
    rejects([&] { batch.append({1, std::string(maxControl + 1, 'x')}); }); batch.flush();
    check(writes.size() == 1 && writes[0] == bytes, "An invalid frame corrupted a pending batch");
}
}
int main() { try { watchBudgets(); limitStatuses(); changeClassification(); commandLines(); resources(); protocol(); frameBatching(); frameBatchBoundaries(); std::cout << "WSL command line, resource and protocol tests passed\n"; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
