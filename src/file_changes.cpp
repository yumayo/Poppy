#include "file_changes.h"
#include <string>

namespace poppy {
FileChange classifyFileChange(std::string_view path, bool topology, bool windows) {
    std::string normalized;
    if (windows) {
        normalized = path;
        for (auto& c : normalized) { if (c == '\\') c = '/'; else if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; }
        path = normalized;
    }
    size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        if (path.substr(start, end == path.npos ? end : end - start) == ".git") {
            if (end == path.npos) return {true, topology, false};
            const auto name = path.substr(end + 1);
            if (name.size() >= 5 && name.substr(name.size() - 5) == ".lock") return {};
            const bool relevant = name == "HEAD" || (windows && name == "head") || name == "index" || name == "config" || name == "packed-refs" ||
                name == "commondir" || name == "gitdir" || name == "refs" || name.substr(0, 5) == "refs/";
            return {relevant, false, false};
        }
        if (end == path.npos) break;
        start = end + 1;
    }
    return {true, topology, true};
}
}
