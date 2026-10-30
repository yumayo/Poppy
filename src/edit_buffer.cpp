#include "edit_buffer.h"
#include <algorithm>

namespace poppy {
namespace {
enum class WordClass { Space, Word, Symbol };
WordClass wordClass(std::string_view text, size_t offset) {
    const auto c = decodeUtf8(text, offset);
    if (c <= U' ' || c == 0x85 || c == 0xa0 || c == 0x1680 ||
        (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
        c == 0x202f || c == 0x205f || c == 0x3000) return WordClass::Space;
    if ((c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') ||
        (c >= U'0' && c <= U'9') || c == U'_') return WordClass::Word;
    // Keep non-ASCII text together, separating common punctuation and emoji.
    if (c >= 0x80 && !(c >= 0x2000 && c <= 0x206f) &&
        !(c >= 0x3000 && c <= 0x303f) && !(c >= 0xff01 && c <= 0xff0f) &&
        !(c >= 0xff1a && c <= 0xff20) && !(c >= 0xff3b && c <= 0xff40) &&
        !(c >= 0xff5b && c <= 0xff65) && !(c >= 0x1f000 && c <= 0x1faff)) return WordClass::Word;
    return WordClass::Symbol;
}
}
WordRange wordRange(std::string_view line, size_t offset) {
    offset = std::min(offset, line.size());
    if (offset == line.size()) return {offset, offset};
    while (offset && (static_cast<unsigned char>(line[offset]) & 0xc0) == 0x80) --offset;
    const auto kind = wordClass(line, offset);
    size_t begin = offset, end = offset;
    while (begin) {
        size_t previous = begin - 1;
        while (previous && (static_cast<unsigned char>(line[previous]) & 0xc0) == 0x80) --previous;
        if (wordClass(line, previous) != kind) break;
        begin = previous;
    }
    do { decodeUtf8(line, end); } while (end < line.size() && wordClass(line, end) == kind);
    return {begin, end};
}
void EditBuffer::initialize(std::string bytes) {
    baseline = std::move(bytes); savedText_ = document.text;
    bom = baseline.compare(0, 3, "\xef\xbb\xbf") == 0;
    undo_.clear(); redo_.clear(); historyBytes_ = 0; revision = 0;
}
void EditBuffer::saved(std::string bytes, std::string text) { baseline = std::move(bytes); savedText_ = std::move(text); }
size_t EditBuffer::previous(size_t p) const {
    p = std::min(p, document.text.size()); if (!p) return 0;
    if (p >= 2 && document.text.compare(p - 2, 2, "\r\n") == 0) return p - 2;
    --p; while (p && (static_cast<unsigned char>(document.text[p]) & 0xc0) == 0x80) --p;
    return p;
}
size_t EditBuffer::next(size_t p) const {
    p = std::min(p, document.text.size());
    if (document.text.compare(p, 2, "\r\n") == 0) return p + 2;
    if (p < document.text.size()) decodeUtf8(document.text, p);
    return p;
}
size_t EditBuffer::previousWord(size_t p) const {
    p = std::min(p, document.text.size());
    while (p && wordClass(document.text, previous(p)) == WordClass::Space) p = previous(p);
    if (p) {
        const auto kind = wordClass(document.text, previous(p));
        do { p = previous(p); } while (p && wordClass(document.text, previous(p)) == kind);
    }
    return p;
}
size_t EditBuffer::nextWord(size_t p) const {
    p = std::min(p, document.text.size());
    if (p < document.text.size()) {
        const auto kind = wordClass(document.text, p);
        do { p = next(p); } while (p < document.text.size() && wordClass(document.text, p) == kind);
    }
    while (p < document.text.size() && wordClass(document.text, p) == WordClass::Space) p = next(p);
    return p;
}
std::string EditBuffer::newline() const {
    const auto p = document.text.find('\n'); return p != std::string::npos && p && document.text[p - 1] == '\r' ? "\r\n" : "\n";
}
EditBuffer::CutRange EditBuffer::cutRange(size_t anchor, size_t caret) const {
    size_t begin = std::min({anchor, caret, document.text.size()});
    size_t end = std::min(std::max(anchor, caret), document.text.size());
    size_t textBegin = begin;
    if (begin == end && !document.lines.empty()) {
        const size_t row = static_cast<size_t>(std::upper_bound(document.lines.begin(), document.lines.end(), begin) - document.lines.begin() - 1);
        textBegin = begin = document.lines[row];
        if (row + 1 < document.lines.size()) end = document.lines[row + 1];
        else { end = document.text.size(); begin = previous(begin); }
    }
    return {begin, end, document.text.substr(textBegin, end - textBegin)};
}
bool EditBuffer::moveLines(size_t& anchor, size_t& caret, bool down) {
    if (document.lines.empty() || anchor > document.text.size() || caret > document.text.size()) return false;
    const auto rowAt = [&](size_t p) { return static_cast<size_t>(std::upper_bound(document.lines.begin(), document.lines.end(), p) - document.lines.begin() - 1); };
    const auto lineEnd = [&](size_t row) { return row + 1 < document.lines.size() ? previous(document.lines[row + 1]) : document.text.size(); };
    const size_t begin = std::min(anchor, caret), end = std::max(anchor, caret);
    const size_t first = rowAt(begin);
    size_t last = rowAt(end);
    // A selection ending at a line's start does not include that line.
    const bool endAtStart = begin != end && end == document.lines[last];
    if (endAtStart) --last;
    if ((!down && first == 0) || (down && last + 1 == document.lines.size())) return false;
    const size_t rangeFirst = down ? first : first - 1, rangeLast = down ? last + 1 : last;
    const size_t replaceBegin = document.lines[rangeFirst], replaceEnd = lineEnd(rangeLast);
    std::string text; text.reserve(replaceEnd - replaceBegin);
    for (size_t row = rangeFirst; row <= rangeLast; ++row) {
        const size_t source = down ? (row == rangeFirst ? rangeLast : row - 1) : (row == rangeLast ? rangeFirst : row + 1);
        text.append(document.text, document.lines[source], lineEnd(source) - document.lines[source]);
        // Keep every existing line separator at its row boundary, including
        // mixed LF/CRLF and a final line without a newline.
        if (row != rangeLast) text.append(document.text, lineEnd(row), document.lines[row + 1] - lineEnd(row));
    }
    const size_t anchorRow = rowAt(anchor), caretRow = rowAt(caret);
    const size_t anchorColumn = anchor - document.lines[anchorRow], caretColumn = caret - document.lines[caretRow];
    if (!replace(replaceBegin, replaceEnd, std::move(text))) return false;
    const auto movedPosition = [&](size_t p, size_t row, size_t column) {
        if (endAtStart && p == end) {
            const size_t following = down ? last + 2 : last;
            return following < document.lines.size() ? static_cast<size_t>(document.lines[following]) : document.text.size();
        }
        const size_t movedRow = down ? row + 1 : row - 1;
        return document.lines[movedRow] + std::min(column, lineEnd(movedRow) - document.lines[movedRow]);
    };
    anchor = movedPosition(anchor, anchorRow, anchorColumn);
    caret = movedPosition(caret, caretRow, caretColumn);
    return true;
}
bool EditBuffer::replace(size_t begin, size_t end, std::string text) {
    if (begin > end || end > document.text.size() || document.text.size() - (end - begin) + text.size() > maxFileBytes) return false;
    const auto boundary = [&](size_t p) { return p == document.text.size() || (static_cast<unsigned char>(document.text[p]) & 0xc0) != 0x80; };
    if (!boundary(begin) || !boundary(end)) return false;
    // Reject binary/invalid clipboard data and bound the line index as on load.
    for (size_t p = 0; p < text.size();) {
        const size_t start = p; const auto c = decodeUtf8(text, p);
        if (!c || (c == 0xfffd && text.compare(start, p - start, "\xef\xbf\xbd") != 0)) return false;
    }
    const auto removed = document.text.substr(begin, end - begin);
    if (removed == text) return true;
    if (std::count(document.text.begin(), document.text.end(), '\n') - std::count(removed.begin(), removed.end(), '\n') + std::count(text.begin(), text.end(), '\n') >= 1000000) return false;
    redo_.clear(); undo_.push_back({begin, removed, std::move(text)});
    const auto& change = undo_.back(); historyBytes_ += change.before.size() + change.after.size();
    document.text.replace(begin, end - begin, change.after); ++revision; indexDocument(document);
    while (undo_.size() > 1 && (historyBytes_ > maxFileBytes || undo_.size() > 1000)) { historyBytes_ -= undo_.front().before.size() + undo_.front().after.size(); undo_.pop_front(); }
    return true;
}
bool EditBuffer::undo(size_t& anchor, size_t& caret) {
    if (undo_.empty()) return false;
    auto change = std::move(undo_.back()); undo_.pop_back();
    historyBytes_ -= change.before.size() + change.after.size();
    document.text.replace(change.begin, change.after.size(), change.before);
    anchor = caret = change.begin + change.before.size(); redo_.push_back(std::move(change)); ++revision; indexDocument(document); return true;
}
bool EditBuffer::redo(size_t& anchor, size_t& caret) {
    if (redo_.empty()) return false;
    auto change = std::move(redo_.back()); redo_.pop_back();
    document.text.replace(change.begin, change.before.size(), change.after);
    anchor = caret = change.begin + change.after.size(); historyBytes_ += change.before.size() + change.after.size();
    undo_.push_back(std::move(change)); ++revision; indexDocument(document); return true;
}
std::string EditBuffer::encoded() const {
    if (document.encoding == "UTF-8") return (bom ? std::string("\xef\xbb\xbf") : std::string{}) + document.text;
    const bool le = document.encoding == "UTF-16 LE";
    std::string bytes = le ? "\xff\xfe" : "\xfe\xff";
    const auto unit = [&](uint16_t c) { bytes += static_cast<char>(le ? c & 255 : c >> 8); bytes += static_cast<char>(le ? c >> 8 : c & 255); };
    for (size_t p = 0; p < document.text.size();) {
        auto c = decodeUtf8(document.text, p);
        if (c <= 0xffff) unit(static_cast<uint16_t>(c));
        else { c -= 0x10000; unit(static_cast<uint16_t>(0xd800 + (c >> 10))); unit(static_cast<uint16_t>(0xdc00 + (c & 1023))); }
    }
    return bytes;
}
}
