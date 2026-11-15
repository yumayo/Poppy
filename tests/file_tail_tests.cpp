#include "file_tail.h"
#include "text.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

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
    // Opening a multi-GB file reads only the complete lines of its first window.
    reads = total = 0;
    const auto opened = readFileWindow(size, 0, [&](uint64_t at, size_t count) {
        check(at + count <= maxFileWindowBytes, "Opening a multi-GB file read past its first window");
        ++reads; total += count;
        std::string bytes(count, 'x');
        for (size_t i = 99 - static_cast<size_t>(at % 100); i < count; i += 100) bytes[i] = '\n';
        return bytes;
    });
    check(reads == 2 && total <= maxFileWindowBytes + 3 && opened.offset == 0 && opened.endOffset == maxFileWindowBytes / 100 * 100 &&
        opened.bytes.size() == opened.endOffset, "Opening a multi-GB file did not read its first complete lines");
    bool failed = false;
    try { readFileWindow(size, requested, [](uint64_t at, size_t count) { return std::string(at ? count - 1 : count, 'x'); }); }
    catch (const std::runtime_error&) { failed = true; }
    check(failed, "A concurrently truncated window silently published an incomplete snapshot");
}
FileLineIndex lineIndex(const std::string& bytes, const FileLineIndex* previous = nullptr) {
    const auto from = previous ? previous->bytes / lineIndexChunkBytes * lineIndexChunkBytes : 0;
    const auto scan = scanFileLines(bytes.size(), from, [&](uint64_t at, size_t count) { return bytes.substr(static_cast<size_t>(at), count); });
    const auto merged = mergeFileLines(previous, scan);
    check(merged.has_value(), "A line count could not be merged");
    return *merged;
}
void lineNumbers() {
    // Entries span several index chunks, so windows start inside chunks.
    const auto text = entries(0, 60000, "\r\n");
    for (const int encoding : {0, 1, 2}) {
        const auto bytes = encoding ? utf16(text, encoding == 1) : text;
        const auto index = lineIndex(bytes);
        check(index.bytes == bytes.size() && index.lines == 60000 && index.checkpoints.size() == bytes.size() / lineIndexChunkBytes + 1, "The line index miscounted newlines");
        check(index.format == (encoding == 0 ? LineFormat::Utf8 : encoding == 1 ? LineFormat::Utf16Le : LineFormat::Utf16Be), "The line index lost the byte order");
        for (const auto requested : {uint64_t{0}, uint64_t{lineIndexChunkBytes}, static_cast<uint64_t>(bytes.size() / 3), static_cast<uint64_t>(bytes.size() / 2 + 7), std::numeric_limits<uint64_t>::max()}) {
            const auto snapshot = window(bytes, requested);
            const auto base = index.linesBefore(snapshot.offset, snapshot.chunkLines);
            // Every complete entry in the window starts with its own number.
            const auto content = decoded(snapshot, encoding == 0 ? "UTF-8" : encoding == 1 ? "UTF-16 LE" : "UTF-16 BE");
            check(base && std::to_string(*base) == content.substr(0, content.find(' ')), "A window's first line number differs from the file");
        }
        const auto last = tail(bytes);
        const auto base = index.linesBefore(last.offset, last.chunkLines);
        check(base && *base == 59900, "The tail's first line number differs from the file");
    }
    // Appending extends the counted prefix without recounting it.
    const auto first = text.substr(0, lineIndexChunkBytes * 3 + 123);
    const auto previous = lineIndex(first);
    const auto extended = lineIndex(text, &previous);
    check(extended.lines == 60000 && extended.checkpoints == lineIndex(text).checkpoints, "An appended line count differs from a full count");
    std::vector<uint64_t> reads;
    scanFileLines(text.size(), lineIndexChunkBytes * 3, [&](uint64_t at, size_t count) { reads.push_back(at); return text.substr(static_cast<size_t>(at), count); });
    check(reads.size() > 1 && reads[0] == 0 && reads[1] == lineIndexChunkBytes * 3, "An appended count rescanned the counted prefix");
    // A changed byte order mark cannot continue an earlier count.
    const auto scan = scanFileLines(utf16(text, true).size(), lineIndexChunkBytes, [&, bytes = utf16(text, true)](uint64_t at, size_t count) { return bytes.substr(static_cast<size_t>(at), count); });
    check(!mergeFileLines(&previous, scan), "A count with a different encoding was merged");
    check(lineIndex("").lines == 0 && lineIndex("x").lines == 0 && lineIndex(std::string(lineIndexChunkBytes, '\n')).checkpoints.size() == 2, "Small files were miscounted");
    // Files above the index limit read no prefix for line numbers.
    const uint64_t size = maxLineIndexBytes + 1;
    readFileWindow(size, size / 2, [&](uint64_t at, size_t count) {
        check(at == 0 || size / 2 - at <= maxFileWindowBytes, "A file above the line index limit read a line count prefix");
        return std::string(count, 'x');
    });
    bool failed = false;
    try { scanFileLines(size, 0, [](uint64_t, size_t count) { return std::string(count, 'x'); }); }
    catch (const std::runtime_error&) { failed = true; }
    check(failed, "A file above the line index limit was scanned");
    failed = false;
    try { scanFileLines(lineIndexChunkBytes * 20, 0, [](uint64_t at, size_t count) { return std::string(at ? count - 1 : count, 'x'); }); }
    catch (const std::runtime_error&) { failed = true; }
    check(failed, "A file truncated while counting kept a partial line count");
}
}
void byteSizes() {
    check(formatByteSize(0) == L"0 B" && formatByteSize(1023) == L"1023 B", "Small sizes stay in bytes");
    check(formatByteSize(1024) == L"1.00 KB" && formatByteSize(1536) == L"1.50 KB", "Kilobytes use two decimals");
    check(formatByteSize(1024 * 1024 - 1) == L"1.00 MB", "Rounding carries into the next unit");
    check(formatByteSize(3ull * 1024 * 1024 * 1024 + 512ull * 1024 * 1024) == L"3.50 GB", "Gigabytes use two decimals");
    check(formatByteSize(5ull << 40) == L"5.00 TB" && formatByteSize(2048ull << 40) == L"2048.00 TB", "Terabytes are the largest unit");
}
void splices() {
    const std::string file = "header\nfirst\nsecond\nthird\n";
    const auto read = [&](const std::string& bytes) { return [&bytes](uint64_t at, size_t count) { return at >= bytes.size() ? std::string{} : bytes.substr(static_cast<size_t>(at), count); }; };
    const auto spliced = [&](const FileSplice& splice, const std::string& bytes, uint64_t size) {
        std::string output;
        copySplicedFile(splice, size, read(bytes), [&](std::string_view part) { output.append(part); });
        return output;
    };
    const auto offset = file.find("first");
    const FileSplice change{offset, file.size(), "first\nsecond\n", u8"最初\n\"二\",番目\n"};
    check(spliced(change, file, file.size()) == "header\n" + change.replacement + "third\n", "A splice changed bytes outside its window");
    check(spliced({0, file.size(), file, ""}, file, file.size()).empty() && spliced({file.size(), file.size(), "", "end\n"}, file, file.size()) == file + "end\n",
        "A splice of the whole file or at its end failed");
    // Copies span several blocks without losing bytes at their boundaries.
    std::string large; for (int i = 0; large.size() < 3 * 1024 * 1024 + 17; ++i) large += std::to_string(i) + '\n';
    const auto middle = large.size() / 2;
    check(spliced({middle, large.size(), large.substr(middle, 5), "x"}, large, large.size()) == large.substr(0, middle) + "x" + large.substr(middle + 5), "A multi-block splice lost bytes");
    const auto conflict = [&](const FileSplice& splice, const std::string& bytes, uint64_t size) {
        std::string output;
        try { copySplicedFile(splice, size, read(bytes), [&](std::string_view part) { output.append(part); }); }
        catch (const std::exception& error) { return std::string(error.what()) == fileSpliceConflict; }
        return false;
    };
    // The file changed size, the window's bytes changed, or bytes were
    // appended after the size was observed.
    check(conflict(change, file + "x", file.size() + 1), "A splice ignored a changed size");
    auto changed = file; changed[offset] = 'F';
    check(conflict(change, changed, changed.size()), "A splice overwrote changed window bytes");
    check(conflict(change, file + "appended\n", file.size()), "A splice dropped bytes appended while copying");
    check(conflict({file.size(), file.size(), "x", ""}, file, file.size()), "A splice past the end was accepted");
    bool cancelled = false;
    try { copySplicedFile({middle, large.size(), large.substr(middle, 5), "x"}, large.size(), read(large), [](std::string_view) {}, [] { return true; }); }
    catch (const std::exception&) { cancelled = true; }
    check(cancelled, "A cancelled splice went on copying");
}
int main() {
    try { lineWindows(); boundedReads(); byteWindows(); lineNumbers(); byteSizes(); splices(); std::cout << "File tail tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
