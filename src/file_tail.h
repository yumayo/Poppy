#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
constexpr size_t tailLineCount = 100;
constexpr size_t maxTailBytes = 1024 * 1024;
constexpr size_t maxFileWindowBytes = 512 * 1024;
// Large files up to this size get exact line numbers from a full newline
// scan. Larger files show byte offsets instead.
constexpr uint64_t maxLineIndexBytes = 4ull * 1024 * 1024 * 1024;
constexpr uint64_t lineIndexChunkBytes = 64 * 1024;
constexpr uint64_t unknownLines = UINT64_MAX;
// Full reads a file up to maxFileBytes. Preview does too, but reads only the
// window around the requested offset of a larger file; offset 0 opens it at
// its beginning. Tail and Window read the end or the window around an offset
// of a file of any size. Partial reads are shown read only.
enum class FileReadMode { Full, Preview, Tail, Window };
enum class LineFormat : uint8_t { Utf8, Utf16Le, Utf16Be };
struct FileTail {
    std::string bytes;
    uint64_t offset = 0, fileBytes = 0, endOffset = 0;
    // Newlines between the line index chunk containing offset and offset.
    uint64_t chunkLines = unknownLines;
};
// Read only the BOM and a bounded suffix, even for multi-GB files. The callback
// reads from an absolute byte offset and may return fewer bytes at EOF.
using ReadFileRange = std::function<std::string(uint64_t, size_t)>;
FileTail readFileTail(uint64_t fileBytes, const ReadFileRange& read);
// Read a bounded region around a raw file byte offset. Complete lines are
// preferred without dropping the requested position; oversized lines remain
// bounded fragments. UTF-16 windows include a synthetic decoding BOM.
FileTail readFileWindow(uint64_t fileBytes, uint64_t offset, const ReadFileRange& read);
// Newline counts for each lineIndexChunkBytes chunk of [from, fileBytes).
// from must be chunk aligned. Throws if the file shrinks or is cancelled.
struct FileLineScan {
    uint64_t from = 0, fileBytes = 0;
    LineFormat format = LineFormat::Utf8;
    std::vector<uint32_t> chunks;
};
FileLineScan scanFileLines(uint64_t fileBytes, uint64_t from, const ReadFileRange& read, const std::function<bool()>& cancelled = {});
struct FileLineIndex {
    uint64_t bytes = 0, lines = 0;
    LineFormat format = LineFormat::Utf8;
    // checkpoints[k] counts the newlines before byte k * lineIndexChunkBytes.
    std::vector<uint64_t> checkpoints{0};
    // Newlines before offset, from FileTail::chunkLines of a window there.
    std::optional<uint64_t> linesBefore(uint64_t offset, uint64_t chunkLines) const;
};
// Appends a scan to previous, or starts over when previous is null. Returns
// nothing when the scan does not continue previous (e.g. a changed BOM).
std::optional<FileLineIndex> mergeFileLines(const FileLineIndex* previous, const FileLineScan& scan);
LineFormat lineFormat(const std::string& encoding);
// An edited window of a large file is saved by copying the file around it:
// [0, offset), replacement, then the bytes after original. The copy refuses
// to start unless the file still has fileBytes and holds original at offset.
struct FileSplice {
    uint64_t offset = 0, fileBytes = 0;
    std::string original, replacement;
};
constexpr const char* fileSpliceConflict = "The file changed outside Poppy. Your edits have been kept. Copy them, then reopen the file before saving again.";
using WriteFileBytes = std::function<void(std::string_view)>;
// currentBytes is the size the caller observed on the file it reads. Throws
// fileSpliceConflict when the file no longer matches, and stops when
// cancelled returns true.
void copySplicedFile(const FileSplice& splice, uint64_t currentBytes, const ReadFileRange& read, const WriteFileBytes& write, const std::function<bool()>& cancelled = {});
// Binary units with two decimals, e.g. "512 B", "1.50 KB", "4.00 GB".
std::wstring formatByteSize(uint64_t bytes);
}
