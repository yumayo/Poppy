#include "text_search.h"
#include <algorithm>
#include <utility>

namespace poppy {
unsigned char LiteralPattern::fold(unsigned char ch) const {
    return !matchCase_ && ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}
void LiteralPattern::assign(std::string query, bool matchCase) {
    matchCase_ = matchCase;
    query_ = std::move(query);
    for (auto& ch : query_) ch = static_cast<char>(fold(static_cast<unsigned char>(ch)));
    prefix_.assign(query_.size(), 0);
    for (size_t i = 1, matched = 0; i < query_.size(); ++i) {
        while (matched && query_[i] != query_[matched]) matched = prefix_[matched - 1];
        if (query_[i] == query_[matched]) ++matched;
        prefix_[i] = matched;
    }
}
std::optional<TextMatch> LiteralPattern::next(std::string_view text, TextSearchCursor& cursor, size_t byteBudget, bool overlapping) const {
    if (empty()) { cursor = {text.size(), 0}; return {}; }
    const size_t end = cursor.offset + std::min(byteBudget, text.size() - std::min(cursor.offset, text.size()));
    while (cursor.offset < end) {
        const auto ch = static_cast<char>(fold(static_cast<unsigned char>(text[cursor.offset++])));
        while (cursor.matched && ch != query_[cursor.matched]) cursor.matched = prefix_[cursor.matched - 1];
        if (ch == query_[cursor.matched]) ++cursor.matched;
        if (cursor.matched == query_.size()) {
            cursor.matched = overlapping ? prefix_.back() : 0;
            return TextMatch{cursor.offset - query_.size(), cursor.offset};
        }
    }
    return {};
}
bool LiteralPattern::consume(unsigned char ch, size_t& matched) const {
    if (empty()) return false;
    const auto value = static_cast<char>(fold(ch));
    while (matched && value != query_[matched]) matched = prefix_[matched - 1];
    if (value == query_[matched]) ++matched;
    if (matched != query_.size()) return false;
    matched = 0; return true;
}
void TextSearch::query(std::string text, bool matchCase) {
    pattern_.assign(std::move(text), matchCase);
    start(0);
}
void TextSearch::start(size_t origin, bool backwards) {
    cursor_ = {}; first_.reset(); last_.reset(); current_.reset();
    origin_ = origin; backwards_ = backwards; count_ = ordinal_ = 0;
    ready_ = pattern_.empty();
}
bool TextSearch::step(std::string_view text, size_t byteBudget, const std::function<bool(TextMatch)>& accept) {
    if (ready_) return true;
    const size_t end = cursor_.offset + std::min(byteBudget, text.size() - std::min(cursor_.offset, text.size()));
    while (cursor_.offset < end) {
        const auto match = pattern_.next(text, cursor_, end - cursor_.offset, static_cast<bool>(accept));
        if (!match) break;
        if (accept && !accept(*match)) continue;
        cursor_.matched = 0; // Only accepted matches consume their full range.
        ++count_; last_ = match;
        if (!first_) first_ = match;
        if (backwards_ ? match->begin < origin_ : !current_ && match->begin >= origin_) {
            current_ = match; ordinal_ = count_;
        }
    }
    if (cursor_.offset >= text.size()) {
        ready_ = true;
        if (!current_ && count_) {
            current_ = backwards_ ? last_ : first_;
            ordinal_ = backwards_ ? count_ : 1;
        }
    }
    return ready_;
}
}
