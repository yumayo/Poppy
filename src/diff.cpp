#include "diff.h"
#include <algorithm>
#include <charconv>
#include <limits>

namespace poppy {
namespace {
bool take(std::string_view& text, std::string_view prefix) {
    if (text.substr(0, prefix.size()) != prefix) return false;
    text.remove_prefix(prefix.size()); return true;
}
bool number(std::string_view& text, uint32_t& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) return false;
    text.remove_prefix(static_cast<size_t>(result.ptr - text.data())); return true;
}
bool range(std::string_view& text, uint32_t& start, uint32_t& count) {
    count = 1;
    if (!number(text, start) || (take(text, ",") && !number(text, count))) return false;
    return (!count || start) && static_cast<uint64_t>(start) + count <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;
}
bool hunk(std::string_view text, uint32_t& before, uint32_t& beforeCount, uint32_t& after, uint32_t& afterCount) {
    return take(text, "@@ -") && range(text, before, beforeCount) && take(text, " +") &&
        range(text, after, afterCount) && take(text, " @@");
}
Language headerLanguage(std::string_view path) {
    // Git quotes paths containing control characters, quotes or backslashes.
    // Only the ASCII filename suffix is needed; escaped directory names can stay encoded.
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path.remove_prefix(1); path.remove_suffix(1);
    } else if (!path.empty() && path.back() == '\t') path.remove_suffix(1);
    const auto slash = path.find_last_of('/');
    if (slash != path.npos) path.remove_prefix(slash + 1);
    if (path == "CMakeLists.txt") return Language::Config;
    const auto dot = path.find_last_of('.');
    return dot == path.npos || dot == 0 ? Language::Plain : languageFor(std::string(path.substr(dot)));
}
}
std::string_view DiffDocument::line(DiffLine source) const {
    return source.number ? patch.line(source.row).substr(1) : std::string_view{};
}
size_t DiffDocument::offset(DiffLine source) const { return patch.lines[source.row] + 1; }
DiffRow DiffDocument::displayRow(size_t index, bool inlineView) const {
    if (!inlineView) return rows[index];
    const auto entry = inlineOrder[index];
    auto row = rows[entry / 2];
    if (row.changed) {
        if (entry % 2) row.before = {};
        else row.after = {};
    }
    return row;
}
size_t DiffDocument::rowForLine(DiffLine source, bool after, bool inlineView) const {
    if (!source.number) return 0;
    for (size_t i = 0; i < rowCount(inlineView); ++i) {
        const auto row = displayRow(i, inlineView);
        const auto candidate = after ? row.after : row.before;
        if (candidate.number && candidate.row == source.row) return i;
    }
    return 0;
}
std::optional<size_t> DiffDocument::inlineRowForText(size_t begin, size_t end) const {
    if (begin >= end || end > patch.text.size() || patch.lines.empty()) return {};
    const auto at = std::upper_bound(patch.lines.begin(), patch.lines.end(), begin);
    const auto patchRow = static_cast<size_t>(at - patch.lines.begin() - 1);
    // Inline order follows patch order, even when split rows pair replacements.
    const auto it = std::lower_bound(inlineOrder.begin(), inlineOrder.end(), patchRow, [this](uint32_t entry, size_t row) {
        const auto& change = rows[entry / 2];
        const auto source = entry % 2 || !change.before.number ? change.after : change.before;
        return (change.heading ? change.heading - 1 : source.row) < row;
    });
    if (it == inlineOrder.end()) return {};
    const size_t index = static_cast<size_t>(it - inlineOrder.begin());
    const auto change = displayRow(index, true);
    const auto source = change.after.number ? change.after : change.before;
    if (!source.number || source.row != patchRow) return {};
    const size_t start = offset(source);
    return begin >= start && end <= start + line(source).size() ? std::optional<size_t>(index) : std::nullopt;
}
size_t DiffDocument::mapRow(size_t index, bool fromInline, bool toInline, bool after) const {
    if (!rowCount(fromInline)) return 0;
    index = std::min(index, rowCount(fromInline) - 1);
    if (fromInline == toInline) return index;
    const auto row = displayRow(index, fromInline);
    if (row.heading) {
        for (size_t i = 0; i < rowCount(toInline); ++i) if (displayRow(i, toInline).heading == row.heading) return i;
    }
    if (!(after ? row.after.number : row.before.number)) after = !after;
    return rowForLine(after ? row.after : row.before, after, toInline);
}
std::string DiffDocument::selectedText(bool after, size_t start, size_t end, bool inlineView) const {
    if (start > end) std::swap(start, end);
    std::string result;
    bool separate = false;
    for (size_t i = 0; i < rowCount(inlineView); ++i) {
        const auto row = displayRow(i, inlineView);
        const auto source = (inlineView ? row.after.number != 0 : after) ? row.after : row.before;
        if (!source.number) continue;
        const size_t begin = offset(source), length = line(source).size();
        if (end <= begin || start > begin + length) continue;
        const size_t first = std::max(start, begin), last = std::min(end, begin + length);
        if (inlineView && separate) result += '\n';
        result.append(patch.text, first, last - first);
        separate = true;
        // A patch's terminating newline also represents an unterminated source
        // line. Git marks that case on the following line.
        const bool terminated = patch.line(source.row + 1) != "\\ No newline at end of file";
        if (end > begin + length && terminated) { result += '\n'; separate = false; }
    }
    return result;
}
void indexDiff(DiffDocument& d, bool fileHeadings) {
    d.patch.language = Language::Plain; indexDocument(d.patch);
    d.rows.clear(); d.inlineOrder.clear(); d.maxColumns = 0;
    std::vector<DiffLine> removed, added;
    auto flush = [&] {
        const size_t first = d.rows.size();
        for (size_t i = 0; i < std::max(removed.size(), added.size()); ++i)
            d.rows.push_back({i < removed.size() ? removed[i] : DiffLine{}, i < added.size() ? added[i] : DiffLine{}, true});
        // Keep every removal before every addition in this replacement block.
        for (size_t i = 0; i < removed.size(); ++i) d.inlineOrder.push_back(static_cast<uint32_t>((first + i) * 2));
        for (size_t i = 0; i < added.size(); ++i) d.inlineOrder.push_back(static_cast<uint32_t>((first + i) * 2 + 1));
        removed.clear(); added.clear();
    };
    uint32_t before = 0, beforeCount = 0, after = 0, afterCount = 0;
    Language beforeLanguage = Language::Plain, afterLanguage = Language::Plain;
    LexState beforeState = LexState::Normal, afterState = LexState::Normal;
    for (size_t row = 0; row < d.patch.lines.size(); ++row) {
        const auto text = d.patch.line(row);
        if (text.substr(0, 11) == "diff --git ") {
            flush(); beforeCount = afterCount = 0;
            beforeLanguage = afterLanguage = Language::Plain;
            beforeState = afterState = LexState::Normal;
        }
        if (!beforeCount && !afterCount) {
            if (text.substr(0, 4) == "--- ") { beforeLanguage = headerLanguage(text.substr(4)); continue; }
            if (text.substr(0, 4) == "+++ ") { afterLanguage = headerLanguage(text.substr(4)); continue; }
        }
        if (fileHeadings && ((!beforeCount && !afterCount) &&
            (text.substr(0, 11) == "diff --git " || text.substr(0, 7) == "Binary " || text.substr(0, 9) == "old mode " ||
             text.substr(0, 9) == "new mode " || text.substr(0, 14) == "new file mode " || text.substr(0, 18) == "deleted file mode " ||
             text.substr(0, 11) == "Empty file " || text.substr(0, 12) == "rename from " || text.substr(0, 10) == "rename to "))) {
            d.inlineOrder.push_back(static_cast<uint32_t>(d.rows.size() * 2));
            d.rows.push_back({{}, {}, false, static_cast<uint32_t>(row + 1)});
            d.maxColumns = std::max(d.maxColumns, static_cast<uint32_t>(std::min(text.size(), maxLineColumns)));
            continue;
        }
        if (text.substr(0, 3) == "@@ ") {
            flush();
            if (!hunk(text, before, beforeCount, after, afterCount)) beforeCount = afterCount = 0;
            // Omitted source lines may change the lexer state. Never carry a
            // comment or string across a hunk boundary whose text is unavailable.
            beforeState = afterState = LexState::Normal;
            continue;
        }
        if ((!beforeCount && !afterCount) || text.empty() || text.front() == '\\') continue;
        const DiffLine oldLine{static_cast<uint32_t>(row), before, beforeLanguage, beforeState},
            newLine{static_cast<uint32_t>(row), after, afterLanguage, afterState};
        const auto source = text.substr(1);
        if (text.front() == '-' && beforeCount) {
            lexLine(source, beforeLanguage, beforeState);
            removed.push_back(oldLine); ++before; --beforeCount;
        } else if (text.front() == '+' && afterCount) {
            lexLine(source, afterLanguage, afterState);
            added.push_back(newLine); ++after; --afterCount;
        } else if (text.front() == ' ' && beforeCount && afterCount) {
            lexLine(source, beforeLanguage, beforeState); lexLine(source, afterLanguage, afterState);
            flush(); d.inlineOrder.push_back(static_cast<uint32_t>(d.rows.size() * 2));
            d.rows.push_back({oldLine, newLine, false}); ++before; ++after; --beforeCount; --afterCount;
        } else { flush(); beforeCount = afterCount = 0; }
        if (!beforeCount && !afterCount) flush();
    }
    flush();
    for (const auto& row : d.rows) for (const auto source : {row.before, row.after}) {
        const auto text = d.line(source); uint32_t columns = 0;
        for (size_t p = 0; p < text.size() && columns < maxLineColumns;) {
            const auto c = decodeUtf8(text, p);
            columns += c == '\t' ? 4 - columns % 4 : std::max(1, cellWidth(c));
        }
        d.maxColumns = std::max(d.maxColumns, columns);
    }
}
}
