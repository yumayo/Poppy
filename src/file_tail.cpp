#include "file_tail.h"
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <stdexcept>

namespace poppy {
namespace {
LineFormat formatOf(const std::string& header) {
    return header.compare(0, 2, "\xff\xfe") == 0 ? LineFormat::Utf16Le : header.compare(0, 2, "\xfe\xff") == 0 ? LineFormat::Utf16Be : LineFormat::Utf8;
}
// bytes must start at an even file offset for UTF-16. A trailing odd byte
// is not a complete code unit.
uint64_t countNewlines(const char* bytes, size_t size, LineFormat format) {
    if (format == LineFormat::Utf8) return static_cast<uint64_t>(std::count(bytes, bytes + size, '\n'));
    const size_t newline = format == LineFormat::Utf16Le ? 0 : 1;
    uint64_t count = 0;
    for (size_t at = 0; at + 1 < size; at += 2) count += bytes[at + newline] == '\n' && bytes[at + 1 - newline] == 0;
    return count;
}
uint64_t chunkLines(uint64_t fileBytes, uint64_t offset, LineFormat format, const ReadFileRange& read) {
    if (fileBytes > maxLineIndexBytes) return unknownLines;
    const auto start = offset / lineIndexChunkBytes * lineIndexChunkBytes;
    if (start == offset) return 0;
    const auto count = static_cast<size_t>(offset - start);
    const auto bytes = read(start, count);
    // A truncated file only loses line numbers, not the visible window.
    return bytes.size() == count ? countNewlines(bytes.data(), bytes.size(), format) : unknownLines;
}
}
LineFormat lineFormat(const std::string& encoding) {
    return encoding == "UTF-16 LE" ? LineFormat::Utf16Le : encoding == "UTF-16 BE" ? LineFormat::Utf16Be : LineFormat::Utf8;
}
std::wstring formatByteSize(uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    static const wchar_t* const units[] = {L"KB", L"MB", L"GB", L"TB"};
    size_t unit = 0; double value = static_cast<double>(bytes) / 1024;
    // Round first so 1023.999 KB becomes 1.00 MB instead of 1024.00 KB.
    while (unit + 1 < std::size(units) && value >= 1024 - 0.005) { value /= 1024; ++unit; }
    wchar_t text[32]; std::swprintf(text, std::size(text), L"%.2f %ls", value, units[unit]);
    return text;
}
FileLineScan scanFileLines(uint64_t fileBytes, uint64_t from, const ReadFileRange& read, const std::function<bool()>& cancelled) {
    if (fileBytes > maxLineIndexBytes) throw std::runtime_error("The file is too large to count its lines.");
    if (from % lineIndexChunkBytes || from > fileBytes) throw std::runtime_error("Invalid line count offset.");
    FileLineScan result; result.from = from; result.fileBytes = fileBytes;
    const auto headerBytes = static_cast<size_t>(std::min<uint64_t>(2, fileBytes));
    const auto header = read(0, headerBytes);
    if (header.size() != headerBytes) throw std::runtime_error("File changed while counting its lines; retry.");
    result.format = formatOf(header);
    result.chunks.reserve(static_cast<size_t>((fileBytes - from + lineIndexChunkBytes - 1) / lineIndexChunkBytes));
    constexpr uint64_t block = 16 * lineIndexChunkBytes;
    for (uint64_t at = from; at < fileBytes; at += block) {
        if (cancelled && cancelled()) throw std::runtime_error("Line counting was cancelled.");
        const auto count = static_cast<size_t>(std::min(block, fileBytes - at));
        const auto bytes = read(at, count);
        if (bytes.size() != count) throw std::runtime_error("File changed while counting its lines; retry.");
        for (size_t chunk = 0; chunk < count; chunk += lineIndexChunkBytes)
            result.chunks.push_back(static_cast<uint32_t>(countNewlines(bytes.data() + chunk, std::min<size_t>(lineIndexChunkBytes, count - chunk), result.format)));
    }
    return result;
}
std::optional<uint64_t> FileLineIndex::linesBefore(uint64_t offset, uint64_t inChunk) const {
    const auto chunk = offset / lineIndexChunkBytes;
    if (inChunk == unknownLines || chunk >= checkpoints.size()) return std::nullopt;
    return checkpoints[static_cast<size_t>(chunk)] + inChunk;
}
std::optional<FileLineIndex> mergeFileLines(const FileLineIndex* previous, const FileLineScan& scan) {
    FileLineIndex result; result.format = scan.format; result.bytes = scan.fileBytes;
    const auto first = scan.from / lineIndexChunkBytes;
    if (scan.from) {
        if (!previous || previous->format != scan.format || scan.from % lineIndexChunkBytes || first >= previous->checkpoints.size()) return std::nullopt;
        result.checkpoints.assign(previous->checkpoints.begin(), previous->checkpoints.begin() + static_cast<std::ptrdiff_t>(first + 1));
    }
    if (scan.chunks.size() != (scan.fileBytes - scan.from + lineIndexChunkBytes - 1) / lineIndexChunkBytes) return std::nullopt;
    result.lines = result.checkpoints.back();
    uint64_t start = scan.from;
    for (const auto count : scan.chunks) {
        result.lines += count; start += lineIndexChunkBytes;
        if (start <= scan.fileBytes) result.checkpoints.push_back(result.lines);
    }
    return result;
}
FileTail readFileTail(uint64_t fileBytes, const ReadFileRange& read) {
    FileTail result; result.fileBytes = result.endOffset = fileBytes;
    const auto header = read(0, static_cast<size_t>(std::min<uint64_t>(3, fileBytes)));
    if (header.size() != std::min<uint64_t>(3, fileBytes)) throw std::runtime_error("File changed while reading its tail; retry.");
    const bool le = header.compare(0, 2, "\xff\xfe") == 0, be = header.compare(0, 2, "\xfe\xff") == 0;
    const size_t unit = le || be ? 2 : 1;
    const size_t bom = unit == 2 ? 2 : header.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    result.offset = std::max<uint64_t>(bom, fileBytes > maxTailBytes ? fileBytes - maxTailBytes : 0);
    if (unit == 2 && result.offset % 2) ++result.offset;
    const auto count = static_cast<size_t>(fileBytes - result.offset);
    auto bytes = read(result.offset, count);
    if (bytes.size() != count) throw std::runtime_error("File changed while reading its tail; retry.");
    // A writer can be in the middle of a UTF-16 code unit or surrogate pair.
    if (unit == 2 && bytes.size() % 2) bytes.pop_back();
    const auto code = [&](size_t at) -> unsigned {
        const auto a = static_cast<unsigned char>(bytes[at]);
        if (unit == 1) return a;
        const auto b = static_cast<unsigned char>(bytes[at + 1]);
        return le ? a | (b << 8) : (a << 8) | b;
    };
    if (unit == 2 && !bytes.empty() && code(bytes.size() - 2) >= 0xd800 && code(bytes.size() - 2) <= 0xdbff) bytes.resize(bytes.size() - 2);
    size_t start = 0, lines = 0;
    size_t end = bytes.size();
    // Like tail -n, a final newline terminates the last line rather than
    // counting the editor's empty EOF row as another log entry.
    if (end && code(end - unit) == '\n') end -= unit;
    for (size_t at = end; at >= unit;) {
        at -= unit;
        if (code(at) == '\n' && ++lines == tailLineCount) { start = at + unit; break; }
    }
    if (!start && result.offset > bom) {
        // Drop the partial first line when complete subsequent lines fit.
        // With a single oversized line, retain its bounded final fragment.
        for (size_t at = 0; at < end; at += unit) {
            if (code(at) == '\n') { start = at + unit; break; }
        }
        if (!start) {
            if (unit == 1) {
                while (start < bytes.size() && (static_cast<unsigned char>(bytes[start]) & 0xc0) == 0x80) ++start;
            } else if (!bytes.empty() && code(0) >= 0xdc00 && code(0) <= 0xdfff) start = unit;
        }
    }
    result.offset += start;
    bytes.erase(0, start);
    // decodeText needs the original UTF-16 byte order even for a suffix.
    if (unit == 2) bytes.insert(0, header.substr(0, 2));
    result.bytes = std::move(bytes);
    result.chunkLines = chunkLines(fileBytes, result.offset, formatOf(header), read);
    return result;
}
FileTail readFileWindow(uint64_t fileBytes, uint64_t requestedOffset, const ReadFileRange& read) {
    FileTail result; result.fileBytes = fileBytes;
    const auto header = read(0, static_cast<size_t>(std::min<uint64_t>(3, fileBytes)));
    if (header.size() != std::min<uint64_t>(3, fileBytes)) throw std::runtime_error("File changed while reading its window; retry.");
    const bool le = header.compare(0, 2, "\xff\xfe") == 0, be = header.compare(0, 2, "\xfe\xff") == 0;
    const size_t unit = le || be ? 2 : 1;
    const size_t bom = unit == 2 ? 2 : header.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    const auto target = std::clamp(requestedOffset, static_cast<uint64_t>(bom), fileBytes);
    const auto lastStart = fileBytes > maxFileWindowBytes ? fileBytes - maxFileWindowBytes : 0;
    result.offset = std::max<uint64_t>(bom, std::min(target > maxFileWindowBytes / 2 ? target - maxFileWindowBytes / 2 : 0, lastStart));
    if (unit == 2 && result.offset % 2) ++result.offset;
    const auto count = static_cast<size_t>(std::min<uint64_t>(maxFileWindowBytes, fileBytes - result.offset));
    auto bytes = read(result.offset, count);
    if (bytes.size() != count) throw std::runtime_error("File changed while reading its window; retry.");
    result.endOffset = result.offset + count;
    const bool eof = result.endOffset == fileBytes;
    if (unit == 2 && bytes.size() % 2) bytes.pop_back();
    const auto code = [&](size_t at) -> unsigned {
        const auto a = static_cast<unsigned char>(bytes[at]);
        if (unit == 1) return a;
        const auto b = static_cast<unsigned char>(bytes[at + 1]);
        return le ? a | (b << 8) : (a << 8) | b;
    };
    if (unit == 2 && !bytes.empty() && code(bytes.size() - 2) >= 0xd800 && code(bytes.size() - 2) <= 0xdbff) bytes.resize(bytes.size() - 2);
    size_t start = 0, end = bytes.size();
    if (result.offset > bom) {
        // Only remove a leading partial line if the requested byte survives.
        for (size_t at = 0; at + unit <= end && result.offset + at + unit <= target; at += unit) {
            if (code(at) == '\n') { start = at + unit; break; }
        }
        if (!start) {
            if (unit == 1) {
                while (start < end && (static_cast<unsigned char>(bytes[start]) & 0xc0) == 0x80) ++start;
            } else if (end && code(0) >= 0xdc00 && code(0) <= 0xdfff) start = unit;
        }
    }
    if (!eof) {
        // Keep complete lines after the target when possible. If the target
        // belongs to an oversized line, retain its fragment instead.
        bool completeLine = false;
        for (size_t at = end; at >= start + unit;) {
            at -= unit;
            if (result.offset + at < target) break;
            if (code(at) == '\n') { end = at + unit; completeLine = true; break; }
        }
        if (!completeLine && unit == 1 && end > start) {
            size_t lead = end - 1;
            while (lead > start && (static_cast<unsigned char>(bytes[lead]) & 0xc0) == 0x80) --lead;
            const auto first = static_cast<unsigned char>(bytes[lead]);
            const size_t length = first >= 0xc2 && first <= 0xdf ? 2 : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 1;
            if (length > end - lead) end = lead;
        }
        result.endOffset = result.offset + end;
    }
    result.offset += start;
    bytes = bytes.substr(start, end - start);
    if (unit == 2) bytes.insert(0, header.substr(0, 2));
    result.bytes = std::move(bytes);
    result.chunkLines = chunkLines(fileBytes, result.offset, formatOf(header), read);
    return result;
}
}
