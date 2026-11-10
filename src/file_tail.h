#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace poppy {
constexpr size_t tailLineCount = 100;
constexpr size_t maxTailBytes = 1024 * 1024;
enum class FileReadMode { Full, Preview, Tail };
struct FileTail {
    std::string bytes;
    uint64_t offset = 0, fileBytes = 0;
};
// Read only the BOM and a bounded suffix, even for multi-GB files. The callback
// reads from an absolute byte offset and may return fewer bytes at EOF.
using ReadFileRange = std::function<std::string(uint64_t, size_t)>;
FileTail readFileTail(uint64_t fileBytes, const ReadFileRange& read);
}
