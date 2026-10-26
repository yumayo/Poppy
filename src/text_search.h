#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
struct TextMatch { size_t begin = 0, end = 0; };
struct TextSearchCursor { size_t offset = 0, matched = 0; };

// UTF-8 literal matching. Case folding applies to ASCII; other bytes are exact.
// A cursor can cross work slices without copying the text.
class LiteralPattern {
public:
    void assign(std::string query, bool matchCase = false);
    bool empty() const { return query_.empty(); }
    std::optional<TextMatch> next(std::string_view text, TextSearchCursor& cursor, size_t byteBudget, bool overlapping = false) const;
private:
    unsigned char fold(unsigned char ch) const;
    std::string query_;
    std::vector<size_t> prefix_;
    bool matchCase_ = false;
};

// Keeps a count and a navigation target, never an array of all occurrences.
// Call step on one stable document revision; restart after any edit.
class TextSearch {
public:
    void query(std::string text, bool matchCase = false);
    void start(size_t origin, bool backwards = false);
    bool step(std::string_view text, size_t byteBudget, const std::function<bool(TextMatch)>& accept = {});
    bool ready() const { return ready_; }
    size_t count() const { return count_; }
    size_t ordinal() const { return ordinal_; }
    const std::optional<TextMatch>& current() const { return current_; }
    const LiteralPattern& pattern() const { return pattern_; }
private:
    LiteralPattern pattern_;
    TextSearchCursor cursor_;
    std::optional<TextMatch> first_, last_, current_;
    size_t origin_ = 0, count_ = 0, ordinal_ = 0;
    bool backwards_ = false, ready_ = true;
};
}
