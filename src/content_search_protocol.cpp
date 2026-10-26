#include "content_search_protocol.h"
#include "resource.h"
#include <algorithm>
#include <stdexcept>

namespace poppy::wsl {
bool validSearchRelative(std::string_view relative) {
    if (relative.empty() || relative.size() > 4096 || relative.front() == '/' || relative.find('\0') != relative.npos || !validUtf8(relative)) return false;
    for (size_t p = 0; p < relative.size();) {
        const auto slash = relative.find('/', p); const auto part = relative.substr(p, slash == relative.npos ? relative.size() - p : slash - p);
        if (part.empty() || part == "." || part == "..") return false;
        p = slash == relative.npos ? relative.size() : slash + 1;
    }
    return relative.back() != '/';
}
Message contentSearchMessage(const ContentSearchOptions& options) {
    return {{"query", options.query}, {"matchCase", options.matchCase ? "1" : "0"}, {"respectIgnore", options.respectIgnore ? "1" : "0"},
        {"resultLimit", std::to_string(std::min(options.resultLimit, contentResultLimit))}, {"resultBytes", std::to_string(std::min(options.resultBytes, contentResultBytes))}};
}
ContentSearchOptions contentSearchOptions(const Message& message, std::string_view exclusions) {
    ContentSearchOptions options; options.query = field(message, "query");
    if (!validContentQuery(options.query)) throw std::runtime_error("Invalid content search query");
    options.matchCase = field(message, "matchCase") == "1"; options.respectIgnore = field(message, "respectIgnore") != "0";
    options.resultLimit = std::min<size_t>(number(message, "resultLimit"), contentResultLimit);
    options.resultBytes = std::min<size_t>(number(message, "resultBytes"), contentResultBytes);
    if (exclusions.size() > 4 * 1024 * 1024) throw std::runtime_error("Too many unsaved file exclusions");
    for (size_t p = 0; p < exclusions.size();) {
        const auto end = exclusions.find('\0', p);
        if (end == exclusions.npos || !validSearchRelative(exclusions.substr(p, end - p))) throw std::runtime_error("Invalid content search exclusion");
        options.excluded.emplace_back(exclusions.substr(p, end - p)); p = end + 1;
    }
    return options;
}
Message contentMatchMessage(const ContentMatch& match) {
    return {{"entry", "1"}, {"relative", match.relative}, {"snippet", match.snippet}, {"line", std::to_string(match.line)},
        {"column", std::to_string(match.column)}, {"begin", std::to_string(match.begin)}, {"end", std::to_string(match.end)}};
}
ContentMatch contentMatchFromMessage(const Message& message) {
    ContentMatch result; result.relative = field(message, "relative"); result.snippet = field(message, "snippet");
    const auto line = number(message, "line"), column = number(message, "column"), begin = number(message, "begin"), end = number(message, "end");
    if (!validSearchRelative(result.relative) || result.snippet.size() > 512 || !validUtf8(result.snippet) ||
        !line || line > maxFileBytes || !column || column > maxFileBytes || begin >= end || end > maxFileBytes)
        throw std::runtime_error("Invalid WSL search match");
    result.line = static_cast<uint32_t>(line); result.column = static_cast<uint32_t>(column); result.begin = static_cast<size_t>(begin); result.end = static_cast<size_t>(end);
    return result;
}
Message contentSummaryMessage(const ContentSearchSummary& summary) {
    return {{"files", std::to_string(summary.files)}, {"matches", std::to_string(summary.matches)}, {"binary", std::to_string(summary.binary)},
        {"large", std::to_string(summary.large)}, {"errors", std::to_string(summary.errors)}, {"limited", summary.limited ? "1" : "0"},
        {"cancelled", summary.cancelled ? "1" : "0"}, {"searchError", summary.error}, {"done", "1"}};
}
ContentSearchSummary contentSummaryFromMessage(const Message& message) {
    ContentSearchSummary result; result.files = number(message, "files"); result.matches = number(message, "matches");
    result.binary = number(message, "binary"); result.large = number(message, "large"); result.errors = number(message, "errors");
    result.limited = field(message, "limited") == "1"; result.cancelled = field(message, "cancelled") == "1";
    result.error = field(message, "searchError"); return result;
}
}
