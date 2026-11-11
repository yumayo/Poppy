#include "file_tail.h"
#include "text.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
FileTail tail(const std::string& bytes) {
    return readFileTail(bytes.size(), [&](uint64_t at, size_t count) { return bytes.substr(static_cast<size_t>(at), count); });
}
FileTail window(const std::string& bytes, uint64_t offset) {
    return readFileWindow(bytes.size(), offset, [&](uint64_t at, size_t count) { return bytes.substr(static_cast<size_t>(at), count); });
}
std::string decoded(const FileTail& snapshot, const std::string& expectedEncoding = "UTF-8") {
    std::string text, encoding, error;
    check(decodeText(snapshot.bytes, text, encoding, error), error.c_str());
    check(encoding == expectedEncoding, "Tail lost the original byte order");
    return text;
}
std::string entries(int first, int last, const std::string& newline = "\n") {
    std::string text;
    for (int i = first; i < last; ++i) text += std::to_string(i) + u8" 日本😀" + newline;
    return text;
}
std::string utf16(const std::string& text, bool le) {
    std::string bytes = le ? "\xff\xfe" : "\xfe\xff";
    const auto unit = [&](unsigned c) {
        bytes += static_cast<char>(le ? c & 255 : c >> 8);
        bytes += static_cast<char>(le ? c >> 8 : c & 255);
    };
    for (size_t at = 0; at < text.size();) {
        auto c = decodeUtf8(text, at);
        if (c > 0xffff) { c -= 0x10000; unit(0xd800 + (c >> 10)); unit(0xdc00 + (c & 1023)); }
        else unit(c);
    }
    return bytes;
}
void lineWindows() {
    for (const std::string newline : {"\n", "\r\n"}) {
        const auto text = entries(0, 240, newline);
        const auto snapshot = tail(text);
        check(snapshot.bytes == entries(140, 240, newline) && snapshot.offset + snapshot.bytes.size() == text.size(), "Final newline changed the last 100 entries");
        auto unterminated = text; unterminated.resize(unterminated.size() - newline.size());
        auto expected = entries(140, 240, newline); expected.resize(expected.size() - newline.size());
        check(tail(unterminated).bytes == expected, "Unterminated last line changed the entry count");
        check(tail(entries(0, 100, newline)).bytes == entries(0, 100, newline), "Exactly 100 entries lost their first line");
        check(tail(entries(0, 3, newline)).bytes == entries(0, 3, newline), "A short log was truncated");
        check(decoded(tail("\xef\xbb\xbf" + text)) == entries(140, 240, newline), "UTF-8 BOM corrupted a tail");
        for (const bool le : {true, false}) {
            const auto bytes = utf16(text, le);
            const auto result = tail(bytes);
            const auto encoding = le ? "UTF-16 LE" : "UTF-16 BE";
            check(decoded(result, encoding) == entries(140, 240, newline) && result.offset % 2 == 0, "UTF-16 CRLF or surrogate boundary corrupted");
            const auto small = utf16(u8"日本😀", le);
            check(decoded(tail(small), encoding) == u8"日本😀", "Short UTF-16 tail lost its BOM");
            check(decoded(tail(small + (le ? std::string("\0\xd8", 2) : std::string("\xd8\0", 2))), encoding) == u8"日本😀", "An incomplete trailing surrogate prevented tailing");
            check(decoded(tail(small + "x"), encoding) == u8"日本😀", "An incomplete UTF-16 code unit prevented tailing");
        }
    }
    check(tail("").bytes.empty() && tail("x").bytes == "x" && tail("\n").bytes == "\n", "Empty or single-line tail failed");
    check(tail(std::string(101, '\n')).bytes == std::string(100, '\n'), "Empty log lines were not counted");
}
void boundedReads() {
    const uint64_t size = 5ull * 1024 * 1024 * 1024 + 17;
    const auto suffix = entries(0, 240, "\r\n");
    size_t readBytes = 0, requests = 0;
    const auto snapshot = readFileTail(size, [&](uint64_t at, size_t count) {
        check(at == 0 || at >= size - maxTailBytes, "Tailing scanned the file body");
        check(count <= maxTailBytes, "Unbounded tail allocation");
        readBytes += count; ++requests;
        std::string bytes(count, 'x');
        const auto suffixAt = size - suffix.size();
        for (size_t i = 0; i < count; ++i) if (at + i >= suffixAt) bytes[i] = suffix[static_cast<size_t>(at + i - suffixAt)];
        return bytes;
    });
    check(requests == 2 && readBytes <= maxTailBytes + 3 && snapshot.fileBytes == size && snapshot.offset > 0xffffffffull, "Multi-GB tail overflowed or read too much");
    check(decoded(snapshot) == entries(140, 240, "\r\n"), "Multi-GB suffix differs from the last 100 lines");
    const std::string longLine(maxTailBytes + 30, 'x');
    check(tail(longLine).bytes.size() == maxTailBytes, "An oversized final line was not bounded");
    check(tail(longLine + "\nlast\n").bytes == "last\n", "A partial first line leaked into the tail");
    check(tail(longLine + "\n").bytes.size() == maxTailBytes, "An oversized terminated line disappeared");
    const auto multibyte = std::string(maxTailBytes + 1, 'x') + u8"😀";
    check(decoded(tail(multibyte)).substr(maxTailBytes - 4) == u8"😀", "UTF-8 suffix was split");
    check(decoded(tail(u8"😀" + std::string(maxTailBytes - 1, 'x'))) == std::string(maxTailBytes - 1, 'x'), "A leading UTF-8 continuation leaked into the tail");
    for (const bool le : {true, false}) {
        const auto bytes = utf16(std::string(maxTailBytes / 2 + 20, 'x') + u8"😀", le);
        const auto result = tail(bytes);
        check(result.bytes.size() <= maxTailBytes + 2 && decoded(result, le ? "UTF-16 LE" : "UTF-16 BE").find(u8"😀") != std::string::npos, "Long UTF-16 tail exceeded its bound");
        const auto split = utf16(u8"😀" + std::string(maxTailBytes / 2 - 1, 'x'), le);
        check(decoded(tail(split), le ? "UTF-16 LE" : "UTF-16 BE") == std::string(maxTailBytes / 2 - 1, 'x'), "A leading low surrogate leaked into the tail");
    }
    bool failed = false;
    try { readFileTail(size, [](uint64_t, size_t count) { return std::string(count ? count - 1 : 0, 'x'); }); }
    catch (const std::runtime_error&) { failed = true; }
    check(failed, "A concurrent truncation silently published an incomplete snapshot");
    failed = false;
    try { readFileTail(size, [](uint64_t at, size_t count) { return std::string(at ? count - 1 : count, 'x'); }); }
    catch (const std::runtime_error&) { failed = true; }
    check(failed, "A short suffix read was accepted");
}
void byteWindows() {
    for (const std::string newline : {"\n", "\r\n"}) {
        const auto text = entries(0, 120000, newline);
        for (const int encoding : {0, 1, 2, 3}) {
            const auto bytes = encoding < 2 ? (encoding ? "\xef\xbb\xbf" + text : text) : utf16(text, encoding == 2);
            const size_t bom = encoding < 2 ? (encoding ? 3 : 0) : 2;
            for (const auto requested : {uint64_t{0}, static_cast<uint64_t>(bytes.size() / 2), static_cast<uint64_t>(bytes.size() - 1), std::numeric_limits<uint64_t>::max()}) {
                const auto snapshot = window(bytes, requested);
                check(snapshot.bytes.size() <= maxFileWindowBytes + 2 && snapshot.endOffset <= bytes.size() && snapshot.offset <= snapshot.endOffset, "Byte window exceeded its range or allocation bound");
                check(snapshot.offset <= std::max<uint64_t>(bom, std::min<uint64_t>(requested, bytes.size())) && snapshot.endOffset >= std::min<uint64_t>(requested, bytes.size()), "Line alignment dropped the requested position");
                const auto source = bytes.substr(snapshot.offset, snapshot.endOffset - snapshot.offset);
                check(snapshot.bytes == (encoding >= 2 ? bytes.substr(0, 2) + source : source), "Byte window lost or duplicated source bytes");
                const auto decodedText = decoded(snapshot, encoding < 2 ? "UTF-8" : encoding == 2 ? "UTF-16 LE" : "UTF-16 BE");
                check(decodedText.find(u8"�") == std::string::npos && text.find(decodedText) != std::string::npos, "Byte window split a character or changed text");
                check(requested || snapshot.offset == bom, "Start jump did not reach the first line");
                check(requested < bytes.size() || snapshot.endOffset == bytes.size(), "EOF jump did not reach the final line");
            }
        }
    }
    const auto line = std::string(maxFileWindowBytes * 3, 'x');
    const auto target = static_cast<uint64_t>(maxFileWindowBytes);
    for (const auto& bytes : {line, line + "\nlast\n", std::string("first\n") + line}) {
        const auto snapshot = window(bytes, target);
        check(snapshot.offset < target && snapshot.endOffset > target && snapshot.bytes.size() <= maxFileWindowBytes, "A very long neighboring line removed the requested fragment");
    }
    // Every possible UTF-8/UTF-16 cut through a multibyte scalar must decode.
    std::string scalars;
    for (size_t i = 0; i < maxFileWindowBytes; ++i) scalars += u8"😀";
    for (size_t padding = 0; padding < 5; ++padding) {
        for (const int encoding : {0, 1, 2}) {
            const auto bytes = encoding ? utf16(scalars, encoding == 1) : scalars;
            const auto snapshot = window(bytes, maxFileWindowBytes + padding);
            const auto content = decoded(snapshot, !encoding ? "UTF-8" : encoding == 1 ? "UTF-16 LE" : "UTF-16 BE");
            check(content.find(u8"�") == std::string::npos && scalars.find(content) != std::string::npos, "A bounded giant-line window split a surrogate or UTF-8 sequence");
        }
    }
    for (const bool le : {true, false}) {
        const auto bytes = utf16(u8"日本😀", le) + (le ? std::string("\0\xd8", 2) : std::string("\xd8\0", 2)) + "x";
        const auto snapshot = window(bytes, std::numeric_limits<uint64_t>::max());
        check(snapshot.endOffset == bytes.size() && decoded(snapshot, le ? "UTF-16 LE" : "UTF-16 BE") == u8"日本😀", "An incomplete UTF-16 write lost EOF following");
    }
    check(window("", 100).bytes.empty() && window("x", 100).bytes == "x", "A window beyond a truncated file failed");
    check(window(std::string(maxFileWindowBytes * 2, '\n'), maxFileWindowBytes).bytes.size() < 1000000, "A byte window exceeded the line index limit");
    const uint64_t size = 7ull * 1024 * 1024 * 1024;
    const uint64_t requested = 5ull * 1024 * 1024 * 1024 + 123;
    size_t reads = 0, total = 0;
    const auto snapshot = readFileWindow(size, requested, [&](uint64_t at, size_t count) {
        check(at == 0 || (at <= requested && requested - at <= maxFileWindowBytes), "A byte window scanned unrelated file contents");
        ++reads; total += count;
        return std::string(count, 'x');
    });
    check(reads == 2 && total <= maxFileWindowBytes + 3 && snapshot.offset > 0xffffffffull && snapshot.endOffset < size, "A multi-GB byte window overflowed or performed unbounded reads");
    bool failed = false;
    try { readFileWindow(size, requested, [](uint64_t at, size_t count) { return std::string(at ? count - 1 : count, 'x'); }); }
    catch (const std::runtime_error&) { failed = true; }
    check(failed, "A concurrently truncated window silently published an incomplete snapshot");
}
}
int main() {
    try { lineWindows(); boundedReads(); byteWindows(); std::cout << "File tail tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
