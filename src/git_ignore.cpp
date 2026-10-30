#include "git_ignore.h"
#include <string_view>

namespace poppy {
namespace {
std::string unquote(std::string_view text) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') return std::string(text);
    std::string path;
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        char c = text[i];
        if (c == '\\' && i + 2 < text.size()) {
            c = text[++i];
            if (c >= '0' && c <= '7') {
                unsigned value = c - '0';
                for (int n = 1; n < 3 && i + 2 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7'; ++n)
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
}
std::unordered_set<std::string> gitIgnoredNames(const std::vector<std::string>& names, const GitIgnoreRun& run) {
    std::unordered_set<std::string> ignored;
    size_t offset = 0;
    while (offset < names.size()) {
        std::vector<std::string> args{"git.exe", "--no-optional-locks", "-c", "core.quotePath=false", "check-ignore", "--"};
        size_t bytes = 0;
        // Stay below Windows command-line and WSL control-frame/argument limits,
        // including quoting and JSON escapes. Never launch one Git per entry.
        while (offset < names.size() && args.size() < 54) {
            const auto& name = names[offset];
            if (bytes && bytes + name.size() + 3 > 8000) break;
            args.push_back("./" + name); bytes += name.size() + 3; ++offset;
        }
        const auto output = run(args);
        if (!output) return {}; // Git missing, outside a worktree, or a failed batch.
        std::string_view remaining(*output);
        while (!remaining.empty()) {
            const auto end = remaining.find('\n');
            if (end == remaining.npos) return {}; // Do not use truncated output.
            auto line = remaining.substr(0, end); remaining.remove_prefix(end + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            auto path = unquote(line);
            if (path.compare(0, 2, "./") == 0) ignored.insert(path.substr(2));
        }
    }
    return ignored;
}
}
