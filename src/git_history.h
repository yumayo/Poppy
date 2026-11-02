#pragma once
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
inline constexpr size_t gitCommitPageSize = 50;
enum class GitDiffKind { Automatic, Changes, Staged, Commit };
struct GitBranch { std::string ref, name, tip; bool current = false; };
struct GitCommit { std::string hash, shortHash, subject, author, date; };
struct GitStatusEntry {
    std::string path, previousPath;
    char index = ' ', worktree = ' ';
    bool conflicted() const;
    bool staged() const { return index != ' ' && index != '?' && !conflicted(); }
    bool changed() const { return worktree != ' ' || conflicted(); }
};
struct GitCommitFile { std::string path; char state = 'M'; std::string previousPath = {}; };
std::vector<GitBranch> parseGitBranches(std::string_view output);
std::vector<GitCommit> parseGitCommits(std::string_view output);
std::vector<GitStatusEntry> parseGitStatus(std::string_view output);
std::vector<GitCommitFile> parseGitCommitFiles(std::string_view output);
std::vector<std::string> gitStatusArguments();
// Read-only commands run in the owning repository; failures return nullopt.
using GitBaseRun = std::function<std::optional<std::string>(const std::vector<std::string>&)>;
// HEAD content for normal editor line backgrounds, including staged changes.
// Added/untracked files use an empty base; ignored/missing/nonregular files and
// Git failures return nullopt. A rename compares with its original HEAD path.
std::optional<std::string> gitEditorBase(const GitStatusEntry& entry, const GitBaseRun& run);
std::vector<std::string> gitLogArguments(const std::string& tip, size_t skip);
std::vector<std::string> gitCommitFilesArguments(const std::string& hash);
// Read the complete file from this commit, independently of the working tree.
std::vector<std::string> gitRevisionFileArguments(const std::string& hash, const std::string& path);
// Include both paths of a rename: filtering on its destination alone makes
// Git treat it as an addition before rename detection can run.
std::vector<std::string> gitDiffArguments(GitDiffKind kind, const std::string& hash, const std::string& path, const std::string& previousPath = {});
// Update only this file's index entries; a rename also includes its source.
std::vector<std::string> gitStageArguments(const GitStatusEntry& entry, bool stage);
// Stage tracked and untracked changes throughout the command's repository.
std::vector<std::string> gitStageAllArguments();
// Reset the repository's entire index without changing HEAD or working files.
// Omitting the revision also supports repositories before their first commit.
std::vector<std::string> gitUnstageAllArguments();
// Commit only the index. A blank amend message keeps the previous message.
// Reject empty normal messages, an empty index, and unresolved conflicts.
std::vector<std::string> gitCommitArguments(const std::vector<GitStatusEntry>& entries, const std::string& message, bool amend);
// Stash unstaged and nonignored untracked files, keeping the index in place.
// Git also records the index snapshot in the stash. No command for a clean
// worktree; reject conflicts before modifying files.
std::vector<std::string> gitStashUnstagedArguments(const std::vector<GitStatusEntry>& entries);
// Stash only the index changes, keeping unstaged and untracked changes.
// Requires Git 2.35+. No command for a clean index; reject merge conflicts.
std::vector<std::string> gitStashStagedArguments(const std::vector<GitStatusEntry>& entries);
// Restore working files from the index, then remove nonignored untracked files.
// Use fresh, complete status entries. Reject conflicts before changing any files;
// do not recurse into submodules or force removal of nested repositories.
std::vector<std::vector<std::string>> gitDiscardAllArguments(const std::vector<GitStatusEntry>& entries);
// Discard only the selected path using fresh status. Keep its index and all
// other paths; reject a conflict in the selected file before changing it.
std::vector<std::vector<std::string>> gitDiscardFileArguments(const std::vector<GitStatusEntry>& entries, const std::string& path);
// A decoded UTF-8 untracked file, represented as an addition in a patch list.
std::string untrackedPatch(const std::string& path, std::string_view text);
}
