#include "content_search.h"
#include "content_search_protocol.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("poppy-content-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { check(fs::create_directory(root), "Cannot create search fixture"); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    void file(const std::string& name, std::string_view text) {
        const auto path = root / fs::u8path(name); fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary); stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        check(stream.good(), "Cannot write search fixture");
    }
};
std::vector<ContentMatch> scan(std::string_view bytes, const std::string& query, size_t chunk = 1, bool canonical = false) {
    ContentSearchOptions options; options.query = query; ContentScanner scanner("file.txt", options, canonical);
    for (size_t p = 0; p < bytes.size(); p += chunk) scanner.feed(bytes.substr(p, chunk));
    scanner.feed({}, true); return scanner.take();
}
void decoding() {
    const std::string text = u8"zero\r\n日本語🙂 needle\r\nNEEDLE\n";
    const auto matches = scan(text, "needle");
    check(matches.size() == 2 && matches[0].line == 2 && matches[0].column == 6, "Unicode scalar columns or CRLF lines failed");
    check(matches[0].begin == text.find("needle") && matches[1].begin == text.find("NEEDLE"), "Decoded byte offsets failed");
    for (const auto& match : matches) check(text.substr(match.begin, match.end - match.begin).size() == 6 && (match.snippet.find("needle") != std::string::npos || match.snippet.find("NEEDLE") != std::string::npos), "Snippet lost a match");
    check(scan(text, u8"語🙂").size() == 1, "Multibyte query across read boundaries failed");
    const std::string le("\xff\xfe\x65\x65\x2c\x67\x3d\xd8\x42\xde\x6e\0\x65\0\x65\0\x64\0\x6c\0\x65\0", 22);
    const auto utf16 = scan(le, "needle");
    check(utf16.size() == 1 && utf16[0].begin == 10 && utf16[0].column == 4, "UTF-16 LE surrogates or offsets failed");
    const std::string be("\xfe\xff\0n\0e\0e\0d\0l\0e", 14);
    check(scan(be, "needle").size() == 1, "UTF-16 BE failed");
    check(scan(std::string("needle\0binary", 13), "needle").empty(), "A late NUL published binary results");
    check(scan(std::string("needle\xff", 7), "needle").empty(), "Malformed UTF-8 published results");
    check(scan(le.substr(0, le.size() - 1), "needle").empty(), "Truncated UTF-16 published results");
    check(scan("\xef\xbb\xbfneedle", "needle")[0].begin == 0, "UTF-8 BOM affected document offsets");
    check(scan("\xef\xbb\xbfneedle", "needle", 1, true)[0].begin == 3, "A literal U+FEFF in a dirty buffer was removed");
    std::string longLine(300000, 'x'); longLine += u8"日本needle";
    const auto longMatch = scan(longLine, "needle", 65536);
    check(longMatch.size() == 1 && longMatch[0].snippet.size() <= 512 && longMatch[0].begin == 300006, "Long lines were retained, truncated, or mislocated");
    std::string japanese; for (int i = 0; i < 256; ++i) japanese += u8"語";
    for (const size_t prefix : {0u, 1u, 2u, 127u, 128u, 129u}) {
        const auto truncated = scan(std::string(prefix, 'x') + japanese, japanese, 37);
        check(truncated.size() == 1 && truncated[0].snippet.size() <= 512, "A long Unicode query exceeded its snippet budget");
        poppy::wsl::contentMatchFromMessage(poppy::wsl::contentMatchMessage(truncated[0]));
    }
}
void traversalAndIgnore() {
    Fixture fixture;
    fixture.file(".gitignore", "\xef\xbb\xbf" "build/\n*.tmp\n!keep.tmp\n/root-only.txt\n**/skip?.txt\n[ab].log\nescaped\\ name.txt\n");
    for (const auto* path : {"one.txt", "nested/two.txt", ".hidden.txt", "keep.tmp", "nested/root-only.txt", "nested/keep.txt"}) fixture.file(path, "needle\n");
    for (const auto* path : {"build/hidden.txt", "other.tmp", "root-only.txt", "skip1.txt", "nested/skip2.txt", "a.log", "b.log", "escaped name.txt", ".git/objects/no", "nested/local.txt"}) fixture.file(path, "needle\n");
    fixture.file("nested/.gitignore", "local.txt\n"); fixture.file("nested/.ignore", "ignored.txt\n"); fixture.file("nested/ignored.txt", "needle");
    fixture.file("binary", std::string("needle\0", 7)); fixture.file("large", std::string(maxFileBytes + 1, 'x'));
    std::error_code error; fs::create_directory_symlink(fixture.root, fixture.root / "loop", error);
    error.clear(); fs::create_symlink(fixture.root / "one.txt", fixture.root / "file-link", error);
    ContentSearchOptions options; options.query = "needle"; options.workers = 4;
    std::set<std::string> paths;
    const auto summary = searchContents(fixture.root, options, [&](std::vector<ContentMatch> batch) { check(batch.size() <= 32, "Unbounded result batch"); for (const auto& match : batch) paths.insert(match.relative); return true; });
    check(summary.error.empty() && summary.errors == 0 && summary.matches == 6 && paths.size() == 6 && summary.binary == 1 && summary.large == 1, "Ignore rules, links, hidden files, or parallel results failed");
    options.excluded = {"one.txt"}; paths.clear();
    const auto excluded = searchContents(fixture.root, options, [&](std::vector<ContentMatch> batch) { for (const auto& match : batch) paths.insert(match.relative); return true; });
    check(excluded.matches == 5 && !paths.count("one.txt"), "Dirty buffer exclusion failed");
    options.respectIgnore = false;
    const auto all = searchContents(fixture.root, options, {});
    check(all.matches > excluded.matches && all.matches < 20, "Disabling ignore rules entered metadata or followed links");
    check(!searchContents(fixture.root / "missing", options, {}).error.empty(), "Missing root did not report an error");
}
void limitsAndCancellation() {
    Fixture fixture; for (int i = 0; i < 40; ++i) fixture.file(std::to_string(i) + ".txt", "needle needle needle needle needle\n");
    ContentSearchOptions options; options.query = "needle"; options.resultLimit = 3;
    size_t count = 0;
    auto summary = searchContents(fixture.root, options, [&](std::vector<ContentMatch> batch) { count += batch.size(); return true; });
    check(count == 3 && summary.matches == 3 && summary.limited, "Concurrent result limit failed");
    options.resultLimit = contentResultLimit; options.resultBytes = 1;
    summary = searchContents(fixture.root, options, {});
    check(summary.matches == 0 && summary.limited, "Result byte budget was ignored");
    options.resultBytes = contentResultBytes;
    std::atomic<unsigned> calls{0}; summary = searchContents(fixture.root, options, {}, [&] { return ++calls >= 25; });
    check(summary.cancelled && calls >= 25 && calls < 1000, "Cancellation kept walking/reading");
    options.query = ""; check(!searchContents(fixture.root, options, {}).error.empty(), "Empty query searched all files");
    options.query = "needle"; summary = searchContents(fixture.root, options, [](std::vector<ContentMatch>) { return false; });
    check(summary.limited && summary.matches <= 32, "Consumer backpressure did not stop the producers");
    fixture.file(".gitignore", std::string(64 * 1024 + 1, 'x'));
    summary = searchContents(fixture.root, options, {});
    check(summary.errors > 0, "An oversized ignore file was silently accepted");
}
void protocol() {
    using namespace poppy::wsl;
    ContentSearchOptions options; options.query = u8"日本🙂"; options.matchCase = true; options.respectIgnore = false;
    const std::string name = u8"資料/back\\slash\nfile.cpp"; std::string excluded = name; excluded += '\0';
    const auto decoded = contentSearchOptions(decodeMessage(encodeMessage(contentSearchMessage(options))), excluded);
    check(decoded.query == options.query && decoded.matchCase && !decoded.respectIgnore && decoded.excluded.size() == 1 && decoded.excluded[0] == name, "Search options changed on the wire");
    ContentMatch match; match.relative = name; match.snippet = u8"日本🙂"; match.begin = 9; match.end = 19; match.line = 2; match.column = 4;
    const auto restored = contentMatchFromMessage(decodeMessage(encodeMessage(contentMatchMessage(match))));
    check(restored.relative == name && restored.begin == 9 && restored.line == 2 && restored.column == 4 && restored.snippet == match.snippet, "WSL result changed offsets or POSIX filenames");
    for (const auto* path : {"../outside", "/absolute", "a/../outside", "a/./x", "a//b", "a/"}) {
        auto message = contentMatchMessage(match); message["relative"] = path;
        bool rejected = false; try { contentMatchFromMessage(message); } catch (const std::exception&) { rejected = true; }
        check(rejected, "A result escaped its project");
    }
    check(!validContentQuery(std::string(513, 'a')) && !validContentQuery("two\nlines") && !validContentQuery("\xff"), "Search accepted an invalid/oversized query");
    ContentSearchSummary summary; summary.files = 2; summary.matches = 3; summary.binary = 4; summary.large = 5; summary.errors = 6; summary.limited = true;
    const auto completed = contentSummaryFromMessage(contentSummaryMessage(summary));
    check(completed.files == 2 && completed.matches == 3 && completed.binary == 4 && completed.large == 5 && completed.errors == 6 && completed.limited, "Search completion lost diagnostics");
}
void workload() {
    Fixture fixture; std::string text(256 * 1024, 'x');
    for (size_t p = 79; p < text.size(); p += 80) text[p] = '\n';
    text += "needle";
    for (unsigned i = 0; i < 64; ++i) fixture.file(std::to_string(i) + ".cpp", text);
    for (const unsigned workers : {1u, 2u, 4u}) {
        ContentSearchOptions options; options.query = "needle"; options.workers = workers;
        size_t count = 0; long long first = -1; std::set<std::thread::id> threads;
        const auto start = std::chrono::steady_clock::now();
        const auto summary = searchContents(fixture.root, options, [&](std::vector<ContentMatch> batch) {
            if (first < 0) first = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            threads.insert(std::this_thread::get_id()); count += batch.size();
            for (const auto& match : batch) check(match.begin == text.size() - 6, "Parallel workload lost a match offset");
            return true;
        });
        check(summary.files == 64 && count == 64 && summary.errors == 0 && !summary.limited, "Parallel workload lost files or exceeded its bounds");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::cout << "16 MiB / 64 files, " << workers << " workers: first=" << first << " ms, total=" << elapsed << " ms, producer threads=" << threads.size() << '\n';
    }
#ifdef __linux__
    rusage usage{}; if (!getrusage(RUSAGE_SELF, &usage)) std::cout << "Workload process peak RSS: " << usage.ru_maxrss << " KiB\n";
#endif
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--workload") workload();
        else { decoding(); traversalAndIgnore(); limitsAndCancellation(); protocol(); }
        std::cout << "Content search tests passed.\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
