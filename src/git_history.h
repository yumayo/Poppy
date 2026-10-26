#pragma once
#include <cstddef>
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
struct GitCommitFile { std::string path; char state = 'M'; };
std::vector<GitBranch> parseGitBranches(std::string_view output);
std::vector<GitCommit> parseGitCommits(std::string_view output);
std::vector<GitStatusEntry> parseGitStatus(std::string_view output);
std::vector<GitCommitFile> parseGitCommitFiles(std::string_view output);
std::vector<std::string> gitLogArguments(const std::string& tip, size_t skip);
std::vector<std::string> gitCommitFilesArguments(const std::string& hash);
std::vector<std::string> gitDiffArguments(GitDiffKind kind, const std::string& hash, const std::string& path);
// Update only this file's index entries; a rename also includes its source.
std::vector<std::string> gitStageArguments(const GitStatusEntry& entry, bool stage);
// A decoded UTF-8 untracked file, represented as an addition in a patch list.
std::string untrackedPatch(const std::string& path, std::string_view text);
}
