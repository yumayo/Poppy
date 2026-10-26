#include "git_history.h"
#include <algorithm>

namespace poppy {
namespace {
bool field(std::string_view& input, std::string& value, char delimiter = '\0') {
    const auto end = input.find(delimiter);
    if (end == input.npos) return false;
    value.assign(input.substr(0, end)); input.remove_prefix(end + 1); return true;
}
}
bool GitStatusEntry::conflicted() const {
    return index == 'U' || worktree == 'U' || (index == 'A' && worktree == 'A') || (index == 'D' && worktree == 'D');
}
std::vector<GitBranch> parseGitBranches(std::string_view output) {
    std::vector<GitBranch> branches;
    while (!output.empty()) {
        GitBranch branch; std::string current;
        if (!field(output, branch.ref) || !field(output, branch.name) || !field(output, branch.tip) || !field(output, current, '\n')) break;
        branch.current = current == "*";
        branches.push_back(std::move(branch));
    }
    return branches;
}
std::vector<GitCommit> parseGitCommits(std::string_view output) {
    std::vector<GitCommit> commits;
    while (!output.empty()) {
        GitCommit commit;
        if (!field(output, commit.hash) || !field(output, commit.shortHash) || !field(output, commit.subject) ||
            !field(output, commit.author) || !field(output, commit.date)) break;
        commits.push_back(std::move(commit));
    }
    return commits;
}
std::vector<GitStatusEntry> parseGitStatus(std::string_view output) {
    std::vector<GitStatusEntry> entries;
    std::string record;
    while (field(output, record)) {
        if (record.size() < 4) continue;
        GitStatusEntry entry; entry.index = record[0]; entry.worktree = record[1]; entry.path = record.substr(3);
        if ((entry.index == 'R' || entry.worktree == 'R' || entry.index == 'C' || entry.worktree == 'C') && !field(output, entry.previousPath)) break;
        entries.push_back(std::move(entry));
    }
    return entries;
}
std::vector<GitCommitFile> parseGitCommitFiles(std::string_view output) {
    std::vector<GitCommitFile> files;
    std::string state, path;
    while (field(output, state) && field(output, path)) {
        if (state.empty() || path.empty()) continue;
        files.push_back({std::move(path), state[0]});
    }
    return files;
}
std::vector<std::string> gitLogArguments(const std::string& tip, size_t skip) {
    return {"git.exe", "--no-optional-locks", "log", "--no-decorate", "--encoding=UTF-8", "-z",
        "--format=%H%x00%h%x00%s%x00%an%x00%aI", "-n" + std::to_string(gitCommitPageSize + 1),
        "--skip=" + std::to_string(skip), tip, "--"};
}
std::vector<std::string> gitCommitFilesArguments(const std::string& hash) {
    return {"git.exe", "--no-optional-locks", "show", "--format=", "--first-parent", "--diff-merges=first-parent",
        "--name-status", "--no-renames", "-z", hash, "--"};
}
std::vector<std::string> gitDiffArguments(GitDiffKind kind, const std::string& hash, const std::string& path) {
    std::vector<std::string> args{"git.exe", "--literal-pathspecs", "--no-optional-locks", "-c", "diff.suppressBlankEmpty=false",
        "-c", "core.quotePath=false", kind == GitDiffKind::Commit ? "show" : "diff", "--patch", "--no-ext-diff", "--no-textconv",
        "--no-renames", "--color=never", "--output-indicator-old=-", "--output-indicator-new=+", "--output-indicator-context= "};
    if (kind == GitDiffKind::Commit) args.insert(args.end(), {"--format=", "--first-parent", "--diff-merges=first-parent", hash});
    else if (kind == GitDiffKind::Staged) args.push_back("--cached");
    else args.push_back("--ours");
    args.push_back("--");
    if (!path.empty()) args.push_back(path);
    return args;
}
std::string untrackedPatch(const std::string& path, std::string_view text) {
    std::string patch = "diff --git a/" + path + " b/" + path + "\nnew file mode 100644\n";
    if (text.find('\0') != text.npos) return patch + "Binary file (untracked)\n";
    if (text.empty()) return patch + "Empty file (untracked)\n";
    const size_t lines = static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + (text.back() != '\n');
    patch += "--- /dev/null\n+++ b/" + path + "\n@@ -0,0 +1," + std::to_string(lines) + " @@\n";
    while (!text.empty()) {
        const auto end = text.find('\n');
        patch += '+'; patch.append(text.substr(0, end)); patch += '\n';
        if (end == text.npos) { patch += "\\ No newline at end of file\n"; break; }
        text.remove_prefix(end + 1);
    }
    return patch;
}
}
