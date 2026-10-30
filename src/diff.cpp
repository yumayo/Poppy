#include "diff.h"
#include <algorithm>
#include <charconv>
#include <iterator>
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
std::string unquotePath(std::string_view text) {
    if (text.empty() || text.front() != '"') return std::string(text);
    std::string path;
    for (size_t i = 1; i < text.size(); ++i) {
        char c = text[i];
        if (c == '"') break;
        if (c == '\\' && i + 1 < text.size()) {
            c = text[++i];
            if (c >= '0' && c <= '7') {
                unsigned value = c - '0';
                for (int n = 1; n < 3 && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7'; ++n)
                    value = value * 8 + text[++i] - '0';
                c = static_cast<char>(value);
            } else switch (c) {
            case 'a': c = '\a'; break; case 'b': c = '\b'; break; case 't': c = '\t'; break;
            case 'n': c = '\n'; break; case 'v': c = '\v'; break; case 'f': c = '\f'; break; case 'r': c = '\r'; break;
            }
        }
        path += c;
    }
    return path;
}
std::string sourcePath(std::string_view text) {
    auto path = unquotePath(text);
    if (path.compare(0, 2, "a/") == 0 || path.compare(0, 2, "b/") == 0) path.erase(0, 2);
    return path;
}
std::string diffHeaderPath(std::string_view text) {
    // Unquoted paths may contain spaces, including " b/". Equal halves are
    // unambiguous and also cover binary, empty and mode-only changes.
    const size_t half = text.size() / 2;
    if (half >= 2 && text.size() % 2 && text[half] == ' ' && text.substr(0, half).substr(2) == text.substr(half + 1).substr(2))
        return sourcePath(text.substr(half + 1));
    if (!text.empty() && text.front() == '"') {
        for (size_t i = 1; i < text.size(); ++i) {
            if (text[i] == '\\') { ++i; continue; }
            if (text[i] == '"' && i + 2 < text.size() && text[i + 1] == ' ') return sourcePath(text.substr(i + 2));
        }
    }
    auto separator = text.rfind(" b/");
    if (separator == text.npos) separator = text.rfind(" \"b/");
    return sourcePath(separator == text.npos ? text : text.substr(separator + 1));
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
const DiffFile* DiffDocument::fileForRow(size_t index, bool inlineView) const {
    if (index >= rowCount(inlineView)) return nullptr;
    const size_t splitRow = inlineView ? inlineOrder[index] / 2 : index;
    const auto it = std::upper_bound(files.begin(), files.end(), splitRow, [](size_t row, const DiffFile& file) { return row < file.row; });
    return it == files.begin() ? nullptr : &*std::prev(it);
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
    d.rows.clear(); d.files.clear(); d.inlineOrder.clear(); d.workingLines.clear(); d.maxColumns = 0;
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
            if (fileHeadings) d.files.push_back({d.rows.size(), diffHeaderPath(text.substr(11))});
        }
        if (!beforeCount && !afterCount) {
            if (text.substr(0, 4) == "--- " || text.substr(0, 4) == "+++ ") {
                auto path = text.substr(4);
                if (!path.empty() && path.back() == '\t') path.remove_suffix(1);
                if (!d.files.empty() && path != "/dev/null") d.files.back().path = sourcePath(path);
                if (text.front() == '-') beforeLanguage = headerLanguage(text.substr(4));
                else afterLanguage = headerLanguage(text.substr(4));
                continue;
            }
            if (!d.files.empty() && (text.substr(0, 10) == "rename to " || text.substr(0, 8) == "copy to "))
                d.files.back().path = unquotePath(text.substr(text.front() == 'r' ? 10 : 8));
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

std::optional<size_t> DiffDocument::workingOffset(size_t offset) const {
    const auto it = std::upper_bound(workingLines.begin(), workingLines.end(), offset,
        [](size_t byte, const WorkingLine& line) { return byte < line.patch; });
    if (it == workingLines.begin()) return {};
    const auto& line = *std::prev(it);
    if (offset > line.patch + line.length) return {};
    return line.file + offset - line.patch;
}
size_t DiffDocument::patchOffset(size_t offset) const {
    const auto it = std::upper_bound(workingLines.begin(), workingLines.end(), offset,
        [](size_t byte, const WorkingLine& line) { return byte < line.file; });
    if (it == workingLines.begin()) return workingLines.empty() ? 0 : workingLines.front().patch;
    const auto& line = *std::prev(it);
    return line.patch + std::min(offset - line.file, line.length);
}
DiffDocument workingDiff(const Document& before, const Document& after, const std::string& path) {
    // Bounded Myers alignment: small edits in large files stay linear. For a
    // wholesale rewrite, show the remaining middle as a replacement instead
    // of spending unbounded time or memory on the UI thread.
    using Match = std::pair<size_t, size_t>;
    std::vector<Match> matches;
    size_t first = 0, oldEnd = before.lines.size(), newEnd = after.lines.size();
    while (first < oldEnd && first < newEnd && before.line(first) == after.line(first)) {
        matches.emplace_back(first, first); ++first;
    }
    while (oldEnd > first && newEnd > first && before.line(oldEnd - 1) == after.line(newEnd - 1)) { --oldEnd; --newEnd; }
    const int n = static_cast<int>(oldEnd - first), m = static_cast<int>(newEnd - first);
    constexpr int distanceLimit = 256, origin = distanceLimit + 1;
    std::vector<int> frontier(2 * origin + 1, 0);
    std::vector<std::vector<int>> trace;
    size_t work = 0;
    bool found = false;
    int distance = 0;
    for (; distance <= distanceLimit && work < 2000000 && !found; ++distance) {
        trace.push_back(frontier);
        for (int diagonal = -distance; diagonal <= distance; diagonal += 2) {
            int x = diagonal == -distance || (diagonal != distance && frontier[origin + diagonal - 1] < frontier[origin + diagonal + 1]) ?
                frontier[origin + diagonal + 1] : frontier[origin + diagonal - 1] + 1;
            int y = x - diagonal;
            while (x < n && y < m && before.line(first + x) == after.line(first + y)) { ++x; ++y; ++work; }
            frontier[origin + diagonal] = x; ++work;
            if (x >= n && y >= m) { found = true; break; }
        }
    }
    if (found) {
        std::vector<Match> middle;
        int x = n, y = m;
        for (int d = distance - 1; d >= 0; --d) {
            const auto& previous = trace[static_cast<size_t>(d)];
            const int diagonal = x - y;
            const int prior = diagonal == -d || (diagonal != d && previous[origin + diagonal - 1] < previous[origin + diagonal + 1]) ? diagonal + 1 : diagonal - 1;
            const int priorX = previous[origin + prior], priorY = priorX - prior;
            while (x > priorX && y > priorY) { --x; --y; middle.emplace_back(first + x, first + y); }
            x = priorX; y = priorY;
        }
        matches.insert(matches.end(), middle.rbegin(), middle.rend());
    }
    for (size_t old = oldEnd, current = newEnd; old < before.lines.size() && current < after.lines.size(); ++old, ++current)
        matches.emplace_back(old, current);
    matches.emplace_back(before.lines.size(), after.lines.size());

    DiffDocument result;
    result.patch.text = "Working tree\n";
    result.files.push_back({0, path}); result.rows.push_back({{}, {}, false, 1}); result.inlineOrder.push_back(0);
    size_t patchRow = 1;
    auto source = [&](const Document& document, size_t row, char prefix, bool working) {
        const auto text = document.line(row);
        const size_t offset = result.patch.text.size() + 1;
        result.patch.text += prefix; result.patch.text.append(text); result.patch.text += '\n';
        if (working) result.workingLines.push_back({offset, document.lines[row], text.size()});
        return DiffLine{static_cast<uint32_t>(patchRow++), static_cast<uint32_t>(row + 1), document.language, document.states[row]};
    };
    size_t old = 0, current = 0;
    for (const auto& match : matches) {
        const size_t start = result.rows.size(), removed = match.first - old, added = match.second - current;
        const size_t count = std::max(removed, added);
        result.rows.resize(start + count);
        for (size_t i = 0; i < count; ++i) result.rows[start + i].changed = true;
        for (size_t i = 0; i < removed; ++i) {
            result.rows[start + i].before = source(before, old++, '-', false);
            result.inlineOrder.push_back(static_cast<uint32_t>((start + i) * 2));
        }
        for (size_t i = 0; i < added; ++i) {
            result.rows[start + i].after = source(after, current++, '+', true);
            result.inlineOrder.push_back(static_cast<uint32_t>((start + i) * 2 + 1));
        }
        if (old < before.lines.size() && current < after.lines.size()) {
            auto original = source(before, old++, ' ', false), modified = original;
            modified.number = static_cast<uint32_t>(current + 1); modified.language = after.language; modified.state = after.states[current];
            result.workingLines.push_back({result.patch.text.size() - before.line(old - 1).size() - 1, after.lines[current], after.line(current).size()});
            ++current;
            result.inlineOrder.push_back(static_cast<uint32_t>(result.rows.size() * 2));
            result.rows.push_back({original, modified, false});
        }
    }
    indexDocument(result.patch);
    result.maxColumns = std::max(before.maxColumns, after.maxColumns);
    return result;
}
}
