#include "text.h"
#include <algorithm>
#include <cctype>
#include <limits>

namespace poppy {
void appendUtf8(std::string& s, char32_t c) {
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) { s += static_cast<char>(0xc0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 63)); }
    else if (c < 0x10000) { s += static_cast<char>(0xe0 | (c >> 12)); s += static_cast<char>(0x80 | ((c >> 6) & 63)); s += static_cast<char>(0x80 | (c & 63)); }
    else { s += static_cast<char>(0xf0 | (c >> 18)); s += static_cast<char>(0x80 | ((c >> 12) & 63)); s += static_cast<char>(0x80 | ((c >> 6) & 63)); s += static_cast<char>(0x80 | (c & 63)); }
}
char32_t decodeUtf8(std::string_view s, size_t& p, bool final) {
    if (p >= s.size()) return 0;
    const auto first = static_cast<unsigned char>(s[p]);
    if (first < 0x80) { ++p; return first; }
    const int n = first >= 0xc2 && first <= 0xdf ? 2 : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!n) { ++p; return 0xfffd; }
    const size_t available = std::min(static_cast<size_t>(n), s.size() - p);
    for (size_t i = 1; i < available; ++i) {
        if ((static_cast<unsigned char>(s[p + i]) & 0xc0) != 0x80) { ++p; return 0xfffd; }
    }
    if (available < static_cast<size_t>(n)) { if (final) ++p; return 0xfffd; }
    char32_t c = first & (0x7f >> n);
    for (int i = 1; i < n; ++i) c = (c << 6) | (static_cast<unsigned char>(s[p + i]) & 63);
    if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && c < 0x10000) || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) { ++p; return 0xfffd; }
    p += n;
    return c;
}
int cellWidth(char32_t c) {
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0xfe00 && c <= 0xfe0f)) return 0;
    return c >= 0x1100 && (c <= 0x115f || c == 0x2329 || c == 0x232a ||
        (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) || (c >= 0xac00 && c <= 0xd7a3) ||
        (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe6f) ||
        (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
        (c >= 0x1f300 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd)) ? 2 : 1;
}
bool decodeText(std::string bytes, std::string& out, std::string& encoding, std::string& error) {
    if (bytes.size() >= 2 && ((static_cast<uint8_t>(bytes[0]) == 0xff && static_cast<uint8_t>(bytes[1]) == 0xfe) ||
        (static_cast<uint8_t>(bytes[0]) == 0xfe && static_cast<uint8_t>(bytes[1]) == 0xff))) {
        const bool le = static_cast<uint8_t>(bytes[0]) == 0xff;
        encoding = le ? "UTF-16 LE" : "UTF-16 BE";
        if (bytes.size() % 2) { error = "Invalid UTF-16 file."; return false; }
        auto unit = [&](size_t p) -> uint16_t { const auto a = static_cast<uint8_t>(bytes[p]), b = static_cast<uint8_t>(bytes[p + 1]); return le ? a | (b << 8) : (a << 8) | b; };
        out.clear(); out.reserve(bytes.size());
        for (size_t p = 2; p < bytes.size(); p += 2) {
            char32_t c = unit(p);
            if (c >= 0xd800 && c <= 0xdbff) {
                if (p + 3 >= bytes.size() || unit(p + 2) < 0xdc00 || unit(p + 2) > 0xdfff) { error = "Invalid UTF-16 surrogate."; return false; }
                c = 0x10000 + ((c - 0xd800) << 10) + (unit(p + 2) - 0xdc00); p += 2;
            } else if (c >= 0xdc00 && c <= 0xdfff) { error = "Invalid UTF-16 surrogate."; return false; }
            if (!c) { error = "Binary files are not displayed."; return false; }
            appendUtf8(out, c);
            if (out.size() > maxFileBytes) { error = "Decoded text exceeds the 16 MiB preview limit."; return false; }
        }
        return true;
    }
    if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
    for (size_t p = 0; p < bytes.size();) {
        const size_t start = p;
        const char32_t c = decodeUtf8(bytes, p);
        if (!c) { error = "Binary files are not displayed."; return false; }
        if (c == 0xfffd && bytes.compare(start, p - start, "\xef\xbf\xbd") != 0) { error = "Use UTF-8 or UTF-16 (with BOM) to preview this file."; return false; }
    }
    encoding = "UTF-8"; out = std::move(bytes); return true;
}
Language languageFor(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".py" || ext == ".pyw") return Language::Python;
    if (ext == ".sh" || ext == ".bash" || ext == ".ps1" || ext == ".psm1") return Language::Shell;
    if (ext == ".json" || ext == ".jsonc") return Language::Json;
    if (ext == ".yaml" || ext == ".yml" || ext == ".toml" || ext == ".ini" || ext == ".cmake") return Language::Config;
    const std::string list = "|.c|.cc|.cpp|.cxx|.h|.hpp|.cs|.js|.jsx|.ts|.tsx|.mjs|.rs|.go|.java|.kt|.swift|.css|.scss|";
    return list.find("|" + ext + "|") != std::string::npos ? Language::CLike : Language::Plain;
}
void lexLine(std::string_view s, Language lang, LexState& state, std::vector<Token>* tokens) {
    if (tokens) tokens->clear();
    if (lang == Language::Plain) return;
    auto emit = [&](size_t a, size_t b, Style style) { if (tokens && b > a) tokens->push_back({a, b, style}); };
    if (lang == Language::Diff) {
        if (!s.empty()) emit(0, s.size(), s[0] == '+' ? Style::String : s[0] == '-' ? Style::Removed : s[0] == '@' ? Style::Keyword : Style::Text);
        return;
    }
    const bool hash = lang == Language::Python || lang == Language::Shell || lang == Language::Config;
    static constexpr std::string_view keywords = "|auto|bool|break|case|catch|char|class|const|constexpr|continue|def|default|delete|do|double|elif|else|enum|except|export|extends|false|False|finally|float|fn|for|from|function|if|impl|import|in|int|interface|let|match|mod|namespace|new|null|None|override|pass|private|protected|pub|public|raise|ref|return|self|static|str|string|struct|super|switch|template|this|throw|trait|true|True|try|type|typedef|typeof|undefined|unsafe|use|using|var|virtual|void|while|with|yield|async|await|";
    size_t p = 0;
    while (p < s.size()) {
        const size_t begin = p;
        if (state != LexState::Normal) {
            const auto closing = state == LexState::BlockComment ? std::string_view("*/") : state == LexState::TripleSingle ? std::string_view("'''") : state == LexState::TripleDouble ? std::string_view("\"\"\"") : std::string_view("`");
            size_t end = s.find(closing, p);
            if (state == LexState::Backtick) {
                while (end != std::string_view::npos && end > 0) {
                    size_t slashes = 0; for (size_t k = end; k > 0 && s[k - 1] == '\\'; --k) ++slashes;
                    if (slashes % 2 == 0) break;
                    end = s.find(closing, end + 1);
                }
            }
            const Style style = state == LexState::BlockComment ? Style::Comment : Style::String;
            if (end == std::string_view::npos) { emit(p, s.size(), style); return; }
            p = end + closing.size(); state = LexState::Normal; emit(begin, p, style); continue;
        }
        if ((hash && s[p] == '#') || (!hash && s.substr(p, 2) == "//")) { emit(p, s.size(), Style::Comment); return; }
        if (!hash && s.substr(p, 2) == "/*") { state = LexState::BlockComment; emit(p, p + 2, Style::Comment); p += 2; continue; }
        if (lang == Language::Python && (s.substr(p, 3) == "'''" || s.substr(p, 3) == "\"\"\"")) {
            state = s[p] == '\'' ? LexState::TripleSingle : LexState::TripleDouble; emit(p, p + 3, Style::String); p += 3; continue;
        }
        if (lang == Language::CLike && s[p] == '`') { state = LexState::Backtick; ++p; emit(begin, p, Style::String); continue; }
        if (s[p] == '\'' || s[p] == '"') {
            const char quote = s[p++];
            while (p < s.size()) { if (s[p] == '\\') p = std::min(p + 2, s.size()); else if (s[p++] == quote) break; }
            emit(begin, p, Style::String); continue;
        }
        if (std::isdigit(static_cast<unsigned char>(s[p]))) {
            while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '.' || s[p] == '_')) ++p;
            emit(begin, p, Style::Number); continue;
        }
        if (std::isalpha(static_cast<unsigned char>(s[p])) || s[p] == '_') {
            while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_')) ++p;
            const std::string word = "|" + std::string(s.substr(begin, p - begin)) + "|";
            emit(begin, p, keywords.find(word) != std::string_view::npos ? Style::Keyword : Style::Text); continue;
        }
        ++p;
    }
}
std::string_view Document::line(size_t row) const {
    if (row >= lines.size()) return {};
    const size_t start = lines[row];
    size_t end = row + 1 < lines.size() ? lines[row + 1] - 1 : text.size();
    if (end > start && text[end - 1] == '\r') --end;
    return std::string_view(text).substr(start, end - start);
}
void indexDocument(Document& d) {
    d.lines.clear(); d.states.clear(); d.maxColumns = 0;
    d.lines.push_back(0);
    for (size_t p = 0; p < d.text.size(); ++p) if (d.text[p] == '\n') d.lines.push_back(static_cast<uint32_t>(p + 1));
    LexState state = LexState::Normal;
    d.states.reserve(d.lines.size());
    for (size_t row = 0; row < d.lines.size(); ++row) {
        auto line = d.line(row); d.states.push_back(state); lexLine(line, d.language, state);
        uint32_t col = 0;
        for (size_t p = 0; p < line.size() && col < maxLineColumns;) { auto c = decodeUtf8(line, p); col += c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c)); }
        d.maxColumns = std::max(d.maxColumns, col);
    }
}
}
