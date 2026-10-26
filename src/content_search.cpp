#include "content_search.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace poppy {
size_t contentMatchBytes(const ContentMatch& match) { return sizeof(ContentMatch) + match.relative.size() + match.snippet.size(); }
bool validContentQuery(std::string_view query) {
    if (query.empty() || query.size() > 2048 || query.find_first_of("\r\n\0", 0, 3) != query.npos) return false;
    size_t units = 0;
    for (size_t p = 0; p < query.size();) {
        const auto before = p; const auto ch = decodeUtf8(query, p);
        if (ch == 0xfffd && query.substr(before, p - before) != "\xef\xbf\xbd") return false;
        units += ch > 0xffff ? 2 : 1;
    }
    return units <= 512;
}
ContentScanner::ContentScanner(std::string relative, const ContentSearchOptions& options, bool decodedUtf8)
    : relative_(std::move(relative)), queryBytes_(options.query.size()),
      limit_(std::min(options.resultLimit, contentResultLimit)), byteLimit_(std::min(options.resultBytes, contentResultBytes)), encoding_(decodedUtf8 ? 1 : 0) {
    pattern_.assign(validContentQuery(options.query) ? options.query : std::string{}, options.matchCase);
    for (const unsigned char ch : options.query) if ((ch & 0xc0) != 0x80) ++queryColumns_;
}
void ContentScanner::hit() {
    if (results_.size() >= limit_) { limited_ = true; return; }
    ContentMatch result; result.relative = relative_; result.line = line_;
    result.column = static_cast<uint32_t>(column_ - queryColumns_ + 1);
    result.begin = offset_ - queryBytes_; result.end = offset_;
    size_t begin = std::max(lineStart_, result.begin > 128 ? result.begin - 128 : 0);
    while (begin < result.begin && (static_cast<unsigned char>(history_[begin % history_.size()]) & 0xc0) == 0x80) ++begin;
    const size_t end = std::min(offset_, begin + 500);
    if (begin > lineStart_) result.snippet = u8"…";
    for (size_t p = begin; p < end; ++p) {
        const auto ch = history_[p % history_.size()];
        result.snippet += static_cast<unsigned char>(ch) < 32 ? ' ' : ch;
    }
    if (end < offset_ && !result.snippet.empty() && (static_cast<unsigned char>(history_[end % history_.size()]) & 0xc0) == 0x80) {
        // A truncated snippet ends before its final, incomplete UTF-8 scalar.
        size_t p = result.snippet.size() - 1;
        while (p && (static_cast<unsigned char>(result.snippet[p]) & 0xc0) == 0x80) --p;
        result.snippet.resize(p);
    }
    if (end < offset_) result.snippet += u8"…";
    const auto size = contentMatchBytes(result);
    if (size > byteLimit_ - std::min(bytes_, byteLimit_)) { limited_ = true; return; }
    bytes_ += size; results_.push_back(std::move(result));
}
void ContentScanner::scalar(char32_t ch, std::string_view bytes) {
    if (!ch) { binary_ = true; return; }
    if (offset_ + bytes.size() > maxFileBytes) { large_ = true; return; }
    ++column_;
    for (unsigned char byte : bytes) {
        history_[offset_ % history_.size()] = static_cast<char>(byte); ++offset_;
        if (pattern_.consume(byte, matched_)) hit();
    }
    if (ch == '\n') { ++line_; column_ = 0; lineStart_ = offset_; matched_ = 0; if (line_ > 1000000) large_ = true; }
}
bool ContentScanner::feed(std::string_view bytes, bool final) {
    if (binary_ || large_ || pattern_.empty()) return false;
    if (bytes.size() > 64 * 1024) {
        for (size_t p = 0; p < bytes.size(); p += 64 * 1024) if (!feed(bytes.substr(p, 64 * 1024))) return false;
        return !final || feed({}, true);
    }
    rawBytes_ += bytes.size();
    if (rawBytes_ > maxFileBytes) { large_ = true; return false; }
    pending_.append(bytes);
    size_t p = 0;
    if (!encoding_) {
        if (pending_.size() < 3 && !final) return true;
        if (pending_.compare(0, 2, "\xff\xfe") == 0) { encoding_ = 2; p = 2; }
        else if (pending_.compare(0, 2, "\xfe\xff") == 0) { encoding_ = 3; p = 2; }
        else { encoding_ = 1; if (pending_.compare(0, 3, "\xef\xbb\xbf") == 0) p = 3; }
    }
    while (p < pending_.size() && !binary_ && !large_) {
        if (encoding_ == 1) {
            const size_t before = p; const auto ch = decodeUtf8(pending_, p, final);
            if (p == before) break;
            const auto encoded = std::string_view(pending_).substr(before, p - before);
            if (ch == 0xfffd && encoded != "\xef\xbf\xbd") { binary_ = true; break; }
            scalar(ch, encoded);
        } else {
            if (pending_.size() - p < 2) break;
            const auto unit = [&](size_t at) {
                const auto a = static_cast<unsigned char>(pending_[at]), b = static_cast<unsigned char>(pending_[at + 1]);
                return static_cast<uint16_t>(encoding_ == 2 ? a | (b << 8) : (a << 8) | b);
            };
            char32_t ch = unit(p); size_t length = 2;
            if (ch >= 0xd800 && ch <= 0xdbff) {
                if (pending_.size() - p < 4) break;
                const auto low = unit(p + 2);
                if (low < 0xdc00 || low > 0xdfff) { binary_ = true; break; }
                ch = 0x10000 + ((ch - 0xd800) << 10) + low - 0xdc00; length = 4;
            } else if (ch >= 0xdc00 && ch <= 0xdfff) { binary_ = true; break; }
            std::string encoded; appendUtf8(encoded, ch); scalar(ch, encoded); p += length;
        }
    }
    pending_.erase(0, p);
    if (final && !pending_.empty()) binary_ = true;
    if (binary_ || large_) { results_.clear(); return false; }
    return true;
}
std::vector<ContentMatch> ContentScanner::take() { return binary_ || large_ ? std::vector<ContentMatch>{} : std::move(results_); }

namespace {
namespace fs = std::filesystem;
using Cancelled = std::function<bool()>;
// No following symlinks/reparse points or blocking on a raced-in FIFO.
class Input {
public:
    explicit Input(const fs::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) { const auto error = GetLastError(); absent = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; return; }
        if (!GetFileInformationByHandle(handle_, &before_) || (before_.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
        size = (static_cast<uint64_t>(before_.nFileSizeHigh) << 32) | before_.nFileSizeLow;
#else
        fd_ = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd_ < 0) { absent = errno == ENOENT || errno == ENOTDIR; return; }
        if (fstat(fd_, &before_) || !S_ISREG(before_.st_mode)) return;
        size = static_cast<uint64_t>(before_.st_size);
#endif
        valid = true;
    }
    ~Input() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) close(fd_);
#endif
    }
    int read(char* bytes, size_t count) {
#ifdef _WIN32
        DWORD got = 0; return ReadFile(handle_, bytes, static_cast<DWORD>(count), &got, nullptr) ? static_cast<int>(got) : -1;
#else
        ssize_t got; do { got = ::read(fd_, bytes, count); } while (got < 0 && errno == EINTR); return static_cast<int>(got);
#endif
    }
    bool unchanged() const {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION after{};
        return GetFileInformationByHandle(handle_, &after) && before_.nFileSizeLow == after.nFileSizeLow && before_.nFileSizeHigh == after.nFileSizeHigh &&
            CompareFileTime(&before_.ftLastWriteTime, &after.ftLastWriteTime) == 0;
#else
        struct stat after{}; return !fstat(fd_, &after) && before_.st_size == after.st_size && before_.st_mtim.tv_sec == after.st_mtim.tv_sec && before_.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
#endif
    }
    bool valid = false, absent = false; uint64_t size = 0;
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE; BY_HANDLE_FILE_INFORMATION before_{};
#else
    int fd_ = -1; struct stat before_{};
#endif
};

// Bounded glob DP: '*' excludes '/', '**' crosses it, and '**/' is optional.
bool glob(std::string_view pattern, std::string_view name) {
    if (pattern.find_first_of("*?[\\") == pattern.npos) return pattern == name;
    std::vector<uint8_t> previous(name.size() + 1), current(name.size() + 1); previous[0] = 1;
    for (size_t p = 0; p < pattern.size();) {
        std::fill(current.begin(), current.end(), 0);
        const char ch = pattern[p++];
        if (ch == '*') {
            const bool recursive = p < pattern.size() && pattern[p] == '*';
            while (p < pattern.size() && pattern[p] == '*') ++p;
            const bool slash = recursive && p < pattern.size() && pattern[p] == '/';
            if (slash) ++p;
            current[0] = previous[0];
            bool reachable = previous[0];
            for (size_t i = 1; i <= name.size(); ++i) {
                if (slash) { reachable = reachable || previous[i]; current[i] = previous[i] || (name[i - 1] == '/' && reachable); }
                else current[i] = previous[i] || (current[i - 1] && (recursive || name[i - 1] != '/'));
            }
        } else {
            bool escaped = ch == '\\' && p < pattern.size();
            if (ch == '[' && !escaped) {
                const size_t close = pattern.find(']', p);
                if (close != pattern.npos) {
                    const bool inverse = p < close && (pattern[p] == '!' || pattern[p] == '^');
                    for (size_t i = 1; i <= name.size(); ++i) {
                        bool match = false;
                        for (size_t j = p + (inverse ? 1 : 0); j < close; ++j) {
                            if (j + 2 < close && pattern[j + 1] == '-') { match = match || (name[i - 1] >= pattern[j] && name[i - 1] <= pattern[j + 2]); j += 2; }
                            else match = match || name[i - 1] == pattern[j];
                        }
                        current[i] = previous[i - 1] && name[i - 1] != '/' && (match != inverse);
                    }
                    p = close + 1; previous.swap(current); continue;
                }
            }
            const char literal = escaped ? pattern[p++] : ch;
            for (size_t i = 1; i <= name.size(); ++i) current[i] = previous[i - 1] && (ch == '?' && !escaped ? name[i - 1] != '/' : name[i - 1] == literal);
        }
        previous.swap(current);
    }
    return previous.back() != 0;
}
struct Rule { std::shared_ptr<const std::string> base; std::string pattern; bool directory = false, negated = false, anchored = false; };
bool ignored(const std::vector<Rule>& rules, const std::string& relative, bool directory) {
    bool result = false;
    for (const auto& rule : rules) {
        if (rule.directory && !directory) continue;
        if (relative.compare(0, rule.base->size(), *rule.base) != 0) continue;
        auto name = std::string_view(relative).substr(rule.base->size());
        if (!rule.anchored) { const auto slash = name.find_last_of('/'); if (slash != name.npos) name.remove_prefix(slash + 1); }
        if (glob(rule.pattern, name)) result = !rule.negated;
    }
    return result;
}
std::string pathKey(std::string path) {
#ifdef _WIN32
    for (auto& ch : path) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
#endif
    return path;
}
bool link(const fs::directory_entry& entry, std::error_code& error) {
    if (entry.is_symlink(error)) return true;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(entry.path().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}
struct Slots { std::mutex mutex; std::condition_variable condition; unsigned used = 0; } slots;
struct Slot {
    bool owned = false;
    explicit Slot(const Cancelled& cancelled) {
        std::unique_lock<std::mutex> lock(slots.mutex);
        while (slots.used == 4 && !cancelled()) slots.condition.wait_for(lock, std::chrono::milliseconds(20));
        if (!cancelled()) { ++slots.used; owned = true; }
    }
    ~Slot() { if (owned) { std::lock_guard<std::mutex> lock(slots.mutex); --slots.used; slots.condition.notify_all(); } }
};
}

ContentSearchSummary searchContents(const fs::path& root, const ContentSearchOptions& options,
    const ContentPublish& publish, const std::function<bool()>& cancelled) {
    ContentSearchSummary summary;
    if (!validContentQuery(options.query)) { summary.error = "Enter a single-line UTF-8 search string (up to 512 UTF-16 units)."; return summary; }
    if (!options.resultLimit || !options.resultBytes) { summary.limited = true; return summary; }
    struct Task { fs::path path; std::string relative; };
    std::mutex mutex, delivery; std::condition_variable condition;
    std::deque<Task> queue;
    std::atomic<bool> stopped{false}, limited{false}; bool finished = false;
    std::atomic<size_t> files{0}, binary{0}, large{0}, errors{0};
    size_t matches = 0, resultBytes = 0;
    const auto halt = [&] { return stopped.load() || (cancelled && cancelled()); };
    const auto limit = std::min(options.resultLimit, contentResultLimit), byteLimit = std::min(options.resultBytes, contentResultBytes);
    const auto worker = [&] {
        Slot slot(halt); if (!slot.owned) return;
        try {
            for (;;) {
                Task task;
                { std::unique_lock<std::mutex> lock(mutex);
                    while (queue.empty() && !finished && !halt()) condition.wait_for(lock, std::chrono::milliseconds(20));
                    if (halt() || (queue.empty() && finished)) return;
                    task = std::move(queue.front()); queue.pop_front(); condition.notify_all();
                }
                Input input(task.path);
                if (!input.valid) { ++errors; continue; }
                if (input.size > maxFileBytes) { ++large; continue; }
                ContentScanner scanner(task.relative, options); std::array<char, 64 * 1024> bytes{};
                bool valid = true;
                for (;;) {
                    if (halt()) return;
                    const int count = input.read(bytes.data(), bytes.size());
                    if (count < 0) { ++errors; valid = false; break; }
                    if (!scanner.feed(std::string_view(bytes.data(), static_cast<size_t>(count)), !count)) break;
                    if (!count) break;
                }
                if (!valid) continue;
                if (scanner.binary()) { ++binary; continue; }
                if (scanner.large()) { ++large; continue; }
                if (!input.unchanged()) { ++errors; continue; }
                ++files;
                auto results = scanner.take();
                std::lock_guard<std::mutex> lock(delivery);
                if (halt()) return;
                std::vector<ContentMatch> batch;
                for (auto& result : results) {
                    const auto size = contentMatchBytes(result);
                    if (matches >= limit || size > byteLimit - resultBytes) { limited = true; stopped = true; break; }
                    ++matches; resultBytes += size; batch.push_back(std::move(result));
                    if (batch.size() == 32) {
                        if (publish && !publish(std::move(batch))) { limited = true; stopped = true; break; }
                        batch.clear();
                    }
                }
                if (!batch.empty() && publish && !publish(std::move(batch))) { limited = true; stopped = true; }
                if (scanner.limited()) { limited = true; stopped = true; }
                if (halt()) { condition.notify_all(); return; }
            }
        } catch (const std::exception&) { ++errors; stopped = true; condition.notify_all(); }
    };
    std::vector<std::thread> workers;
    try {
        for (unsigned i = 0; i < std::clamp(options.workers, 1u, 4u); ++i) workers.emplace_back(worker);
        std::set<std::string> excluded; for (const auto& path : options.excluded) excluded.insert(pathKey(path));
        std::vector<Rule> rules; size_t ruleBytes = 0, directories = 0;
        const auto readRules = [&](const fs::path& path, const std::string& base) {
            Input input(path); if (!input.valid) { if (!input.absent) ++errors; return; }
            if (input.size > 64 * 1024 || ruleBytes + input.size + base.size() > 1024 * 1024) { ++errors; return; }
            std::string contents(static_cast<size_t>(input.size), '\0'); const int count = input.read(contents.data(), contents.size());
            if (count < 0 || static_cast<size_t>(count) != contents.size()) { ++errors; return; }
            ruleBytes += contents.size() + base.size(); const auto sharedBase = std::make_shared<const std::string>(base);
            size_t at = contents.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
            while (at < contents.size()) {
                const auto end = contents.find('\n', at); auto line = contents.substr(at, end == contents.npos ? contents.size() - at : end - at);
                at = end == contents.npos ? contents.size() : end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                while (!line.empty() && line.back() == ' ' && (line.size() < 2 || line[line.size() - 2] != '\\')) line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                if (line.size() > 512 || rules.size() >= 10000 || ruleBytes + sizeof(Rule) > 1024 * 1024) { ++errors; continue; }
                Rule rule; rule.base = sharedBase; ruleBytes += sizeof(Rule);
                if (line[0] == '!') { rule.negated = true; line.erase(0, 1); }
                if (!line.empty() && line.back() == '/') { rule.directory = true; line.pop_back(); }
                rule.anchored = line.find('/') != line.npos;
                if (!line.empty() && line[0] == '/') { rule.anchored = true; line.erase(0, 1); }
                if (!line.empty()) { rule.pattern = std::move(line); rules.push_back(std::move(rule)); }
            }
        };
        if (options.respectIgnore) readRules(root / ".git" / "info" / "exclude", "");
        std::function<void(const fs::path&, const std::string&, unsigned)> walk;
        walk = [&](const fs::path& directory, const std::string& relative, unsigned depth) {
            if (halt()) return;
            if (depth > 128 || ++directories > 100000) { limited = true; return; }
            const size_t previous = rules.size();
            if (options.respectIgnore) {
                const auto base = relative.empty() ? "" : relative + '/';
                readRules(directory / ".gitignore", base); readRules(directory / ".ignore", base);
            }
            std::error_code error; fs::directory_iterator it(directory, error), end;
            if (error) { if (relative.empty()) summary.error = error.message(); else ++errors; rules.resize(previous); return; }
            for (; it != end && !halt(); it.increment(error)) {
                if (error) { ++errors; break; }
                const auto& entry = *it; const auto name = entry.path().filename().u8string();
                if (name == ".git" || name == ".hg" || name == ".svn" || link(entry, error)) continue;
                const auto path = relative.empty() ? name : relative + '/' + name;
                if (path.size() > 4096) { ++errors; continue; }
                bool utf8 = true; for (size_t p = 0; p < path.size();) { const auto before = p; const auto ch = decodeUtf8(path, p); if (ch == 0xfffd && path.compare(before, p - before, "\xef\xbf\xbd")) { utf8 = false; break; } }
                if (!utf8) { ++errors; continue; }
                const bool isDirectory = entry.is_directory(error);
                if (options.respectIgnore && ignored(rules, path, isDirectory)) continue;
                if (isDirectory) { walk(entry.path(), path, depth + 1); continue; }
                if (!entry.is_regular_file(error) || excluded.count(pathKey(path))) continue;
                std::unique_lock<std::mutex> lock(mutex);
                while (queue.size() >= 128 && !halt()) condition.wait_for(lock, std::chrono::milliseconds(20));
                if (!halt()) { queue.push_back({entry.path(), path}); condition.notify_all(); }
            }
            if (error) ++errors;
            rules.resize(previous);
        };
        walk(root, "", 0);
    } catch (const std::exception& error) { summary.error = error.what(); stopped = true; }
    { std::lock_guard<std::mutex> lock(mutex); finished = true; condition.notify_all(); }
    for (auto& thread : workers) thread.join();
    summary.files = files; summary.matches = matches; summary.binary = binary; summary.large = large; summary.errors = errors;
    summary.limited = limited; summary.cancelled = cancelled && cancelled();
    return summary;
}
}
