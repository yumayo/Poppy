#pragma once
#include "text_search.h"
#include "text.h"
#include <array>
#include <filesystem>
#include <memory>

namespace poppy {
constexpr size_t contentResultLimit = 2000, contentResultBytes = 2 * 1024 * 1024;
struct ContentMatch {
    std::string relative, snippet;
    uint32_t line = 1, column = 1; // One-based Unicode scalar column.
    size_t begin = 0, end = 0;     // Byte offsets in the decoded UTF-8 document.
};
struct ContentSearchOptions {
    std::string query;
    bool matchCase = false, respectIgnore = true;
    size_t resultLimit = contentResultLimit, resultBytes = contentResultBytes;
    unsigned workers = 2;
    std::vector<std::string> excluded; // Dirty buffers are searched on the UI thread.
};
struct ContentSearchSummary {
    size_t files = 0, matches = 0, binary = 0, large = 0, errors = 0;
    bool limited = false, cancelled = false;
    std::string error;
};
using ContentPublish = std::function<bool(std::vector<ContentMatch>)>;
size_t contentMatchBytes(const ContentMatch& match);
bool validContentQuery(std::string_view query);

// Incremental, bounded decoder/matcher, also used for live dirty buffers.
// Results stay local until validation finishes, so binary/invalid files never
// publish partial matches. decodedUtf8 preserves a literal initial U+FEFF.
class ContentScanner {
public:
    ContentScanner(std::string relative, const ContentSearchOptions& options, bool decodedUtf8 = false);
    bool feed(std::string_view bytes, bool final = false);
    bool binary() const { return binary_; }
    bool large() const { return large_; }
    bool limited() const { return limited_; }
    std::vector<ContentMatch> take();
private:
    void scalar(char32_t ch, std::string_view bytes);
    void hit();
    LiteralPattern pattern_;
    std::string relative_, pending_;
    std::vector<ContentMatch> results_;
    std::array<char, 4096> history_{};
    size_t queryBytes_ = 0, queryColumns_ = 0, offset_ = 0, lineStart_ = 0, matched_ = 0;
    size_t bytes_ = 0, rawBytes_ = 0, limit_ = 0, byteLimit_ = 0;
    uint32_t line_ = 1, column_ = 0;
    int encoding_ = 0; // 0: detect BOM, 1: UTF-8, 2: UTF-16 LE, 3: UTF-16 BE.
    bool binary_ = false, large_ = false, limited_ = false;
};

// File enumeration uses a bounded queue. Concurrent searches share four file
// worker slots per process; each request uses at most four of those slots.
ContentSearchSummary searchContents(const std::filesystem::path& root, const ContentSearchOptions& options,
    const ContentPublish& publish, const std::function<bool()>& cancelled = {});
}
