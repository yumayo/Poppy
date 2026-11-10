#include "file_tail.h"
#include <algorithm>
#include <stdexcept>

namespace poppy {
FileTail readFileTail(uint64_t fileBytes, const ReadFileRange& read) {
    FileTail result; result.fileBytes = fileBytes;
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
    return result;
}
}
