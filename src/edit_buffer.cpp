#include "edit_buffer.h"
#include <algorithm>

namespace poppy {
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
std::string EditBuffer::newline() const {
    const auto p = document.text.find('\n'); return p != std::string::npos && p && document.text[p - 1] == '\r' ? "\r\n" : "\n";
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
