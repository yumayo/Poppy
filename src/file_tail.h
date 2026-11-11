#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace poppy {
constexpr size_t tailLineCount = 100;
constexpr size_t maxTailBytes = 1024 * 1024;
constexpr size_t maxFileWindowBytes = 512 * 1024;
enum class FileReadMode { Full, Preview, Tail, Window };
struct FileTail {
    std::string bytes;
    uint64_t offset = 0, fileBytes = 0, endOffset = 0;
};
// Read only the BOM and a bounded suffix, even for multi-GB files. The callback
// reads from an absolute byte offset and may return fewer bytes at EOF.
using ReadFileRange = std::function<std::string(uint64_t, size_t)>;
FileTail readFileTail(uint64_t fileBytes, const ReadFileRange& read);
// Read a bounded region around a raw file byte offset. Complete lines are
// preferred without dropping the requested position; oversized lines remain
// bounded fragments. UTF-16 windows include a synthetic decoding BOM.
FileTail readFileWindow(uint64_t fileBytes, uint64_t offset, const ReadFileRange& read);
}
