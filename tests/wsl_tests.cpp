#include "resource.h"
#include "wsl_protocol.h"
#include "command_line.h"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
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
int main() { try { commandLines(); resources(); protocol(); frameBatching(); frameBatchBoundaries(); std::cout << "WSL command line, resource and protocol tests passed\n"; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
