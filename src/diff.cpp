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
}
std::string_view DiffDocument::line(DiffLine source) const {
    return source.number ? patch.line(source.row).substr(1) : std::string_view{};
}
size_t DiffDocument::offset(DiffLine source) const { return patch.lines[source.row] + 1; }
std::string DiffDocument::selectedText(bool after, size_t start, size_t end) const {
    if (start > end) std::swap(start, end);
    std::string result;
    for (const auto& row : rows) {
        const auto source = after ? row.after : row.before;
        if (!source.number) continue;
        const size_t begin = offset(source), length = line(source).size();
        if (end <= begin || start > begin + length) continue;
        const size_t first = std::max(start, begin), last = std::min(end, begin + length);
        result.append(patch.text, first, last - first);
        // A patch's terminating newline also represents an unterminated source
        // line. Git marks that case on the following line.
        const bool terminated = patch.line(source.row + 1) != "\\ No newline at end of file";
        if (end > begin + length && terminated) result += '\n';
    }
    return result;
}
void indexDiff(DiffDocument& d) {
    d.patch.language = Language::Plain; indexDocument(d.patch);
    d.rows.clear(); d.maxColumns = 0;
    std::vector<DiffLine> removed, added;
    auto flush = [&] {
        for (size_t i = 0; i < std::max(removed.size(), added.size()); ++i)
            d.rows.push_back({i < removed.size() ? removed[i] : DiffLine{}, i < added.size() ? added[i] : DiffLine{}, true});
        removed.clear(); added.clear();
    };
    uint32_t before = 0, beforeCount = 0, after = 0, afterCount = 0;
    for (size_t row = 0; row < d.patch.lines.size(); ++row) {
        const auto text = d.patch.line(row);
        if (text.substr(0, 3) == "@@ ") {
            flush();
            if (!hunk(text, before, beforeCount, after, afterCount)) beforeCount = afterCount = 0;
            continue;
        }
        if ((!beforeCount && !afterCount) || text.empty() || text.front() == '\\') continue;
        const DiffLine oldLine{static_cast<uint32_t>(row), before}, newLine{static_cast<uint32_t>(row), after};
        if (text.front() == '-' && beforeCount) { removed.push_back(oldLine); ++before; --beforeCount; }
        else if (text.front() == '+' && afterCount) { added.push_back(newLine); ++after; --afterCount; }
        else if (text.front() == ' ' && beforeCount && afterCount) {
            flush(); d.rows.push_back({oldLine, newLine, false}); ++before; ++after; --beforeCount; --afterCount;
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
