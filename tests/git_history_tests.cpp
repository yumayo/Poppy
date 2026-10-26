#include "git_history.h"
#include "diff.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string nul(std::initializer_list<std::string> fields) {
    std::string result;
    for (const auto& field : fields) { result += field; result += '\0'; }
    return result;
}
void statusGroups() {
    const auto entries = parseGitStatus(nul({"MM src/日本語.cpp", "A  added.txt", " D deleted.txt", "?? new.txt", "R  new name.txt", "old name.txt", "UU conflict.txt", "AA both.txt"}));
    check(entries.size() == 7, "Status parsing dropped entries or treated rename sources as entries");
    check(entries[0].staged() && entries[0].changed() && entries[0].path == "src/日本語.cpp", "Partially staged files must appear in both groups");
    check(entries[1].staged() && !entries[1].changed(), "A staged addition leaked into Changes");
    check(!entries[2].staged() && entries[2].changed(), "An unstaged deletion leaked into Staged Changes");
    check(!entries[3].staged() && entries[3].changed(), "An untracked file was not included in Changes");
    check(entries[4].previousPath == "old name.txt" && entries[4].staged(), "Rename source or destination was lost");
    check(!entries[5].staged() && entries[5].changed() && entries[6].conflicted(), "Conflicts were classified as staged changes");
    check(parseGitStatus("M  incomplete").empty(), "Incomplete status output became a file");
}
void records() {
    auto output = nul({"refs/heads/main", "main", "abc"}) + "*\n" + nul({"refs/remotes/origin/topic", "origin/topic", "def"}) + " \n";
    const auto branches = parseGitBranches(output);
    check(branches.size() == 2 && branches[0].current && !branches[1].current && branches[1].ref == "refs/remotes/origin/topic", "Branch refs or current marker were lost");
    const auto commits = parseGitCommits(nul({"abc", "a", "日本語 subject\twith spaces", "Author", "2026-10-26T12:00:00+00:00", "def", "d", "another", "Other", "2026-10-25T12:00:00+00:00"}));
    check(commits.size() == 2 && commits[0].subject == "日本語 subject\twith spaces" && commits[1].hash == "def", "NUL-delimited commit records lost metadata");
    const auto files = parseGitCommitFiles(nul({"A", "src/new.txt", "D", "old.txt", "M", "path\nwith newline.txt"}));
    check(files.size() == 3 && files[1].state == 'D' && files[2].path == "path\nwith newline.txt", "Commit files lost states or unusual paths");
}
void patchLists() {
    DiffDocument diff;
    diff.patch.text = "diff --git a/one b/one\n@@ -1 +1 @@\n-old\n+new\n" + untrackedPatch("two 日本語.txt", "hello\nlast") +
        "diff --git a/bin b/bin\nBinary files a/bin and b/bin differ\n" + untrackedPatch("empty", "");
    indexDiff(diff, true);
    size_t headings = 0;
    for (size_t i = 0; i < diff.rows.size(); ++i) {
        if (diff.rows[i].heading) ++headings;
        check(diff.mapRow(diff.mapRow(i, false, true, true), true, false, true) == i, "File headings lost their position when switching layouts");
    }
    check(headings == 8 && diff.rows.size() == 11, "A patch list omitted file boundaries, binary files or empty files");
    check(diff.selectedText(true, 0, diff.patch.text.size()) == "new\nhello\nlast", "Patch list selection copied metadata or lost added file text");
    const auto empty = untrackedPatch("empty", ""), binary = untrackedPatch("image", std::string_view("\0", 1));
    check(empty.find("@@") == empty.npos && binary.find("Binary") != binary.npos, "Empty or binary additions were rendered as source text");
}
#ifndef _WIN32
namespace fs = std::filesystem;
struct Repository {
    fs::path path = fs::temp_directory_path() / ("poppy-git-history-" + std::to_string(getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Repository() { fs::create_directories(path); }
    ~Repository() { std::error_code error; fs::remove_all(path, error); }
    std::string git(std::vector<std::string> args) const {
        if (!args.empty() && args.front() == "git.exe") args.erase(args.begin());
        args.insert(args.begin(), {"git", "-C", path.string()});
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        int pipes[2]; check(pipe(pipes) == 0, "Cannot create Git output pipe");
        const auto child = fork(); check(child >= 0, "Cannot fork Git");
        if (!child) {
            close(pipes[0]); dup2(pipes[1], STDOUT_FILENO); dup2(pipes[1], STDERR_FILENO); close(pipes[1]);
            execvp(argv[0], argv.data()); _exit(127);
        }
        close(pipes[1]); std::string output; char buffer[4096]; ssize_t count;
        while ((count = read(pipes[0], buffer, sizeof(buffer))) > 0) output.append(buffer, static_cast<size_t>(count));
        close(pipes[0]); int status = 0; waitpid(child, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("Git fixture command failed: " + output);
        return output;
    }
    void write(const std::string& name, const std::string& text) const {
        fs::create_directories((path / name).parent_path());
        std::ofstream file(path / name, std::ios::binary); file << text; file.close(); check(!file.fail(), "Cannot write Git fixture");
    }
    std::string readFile(const std::string& name) const {
        std::ifstream file(path / name, std::ios::binary);
        check(file.is_open(), "Cannot read Git fixture");
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    std::string hash() const { auto result = git({"rev-parse", "HEAD"}); result.pop_back(); return result; }
};
void repositoryStaging() {
    Repository repo;
    repo.git({"init", "--quiet", "--template=", "--initial-branch=main"});
    repo.git({"config", "user.name", "Poppy Test"}); repo.git({"config", "user.email", "poppy@example.invalid"});
    repo.git({"config", "core.autocrlf", "false"}); repo.git({"config", "commit.gpgsign", "false"});
    const std::string file = "src/日本語 [1].txt", other = "src/日本語 1.txt", renamed = "src/renamed [2].txt";
    const auto entry = [&](const std::string& path) {
        const auto entries = parseGitStatus(repo.git({"status", "--porcelain=v1", "-z", "--untracked-files=all"}));
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const GitStatusEntry& value) { return value.path == path; });
        check(found != entries.end(), "A changed file disappeared from Git status");
        return *found;
    };
    repo.write(file, "initial\n"); repo.write(other, "other\n");
    repo.git(gitStageArguments(entry(file), true));
    check(entry(file).staged() && !entry(file).changed() && !entry(other).staged(), "Staging an untracked literal path also staged another file");
    repo.git(gitStageArguments(entry(file), false));
    check(!entry(file).staged() && entry(file).changed() && repo.readFile(file) == "initial\n", "Unstaging before the first commit lost the new file");
    repo.git({"add", "."}); repo.git({"commit", "-qm", "initial"});

    repo.write(file, "staged\n"); repo.write(other, "unrelated\n");
    repo.git(gitStageArguments(entry(file), true));
    repo.write(file, "working\n");
    check(entry(file).staged() && entry(file).changed(), "The staging fixture was not partially staged");
    repo.git(gitStageArguments(entry(file), false));
    check(!entry(file).staged() && entry(file).changed() && repo.readFile(file) == "working\n" && !entry(other).staged(), "Unstaging a partially staged file changed working files or another index entry");
    repo.git(gitStageArguments(entry(file), true));
    const auto patch = repo.git(gitDiffArguments(GitDiffKind::Staged, {}, {}));
    check(entry(file).staged() && !entry(file).changed() && patch.find("+working") != patch.npos && patch.find("unrelated") == patch.npos,
        "Staging did not include the entire selected file or included unrelated changes");
    fs::remove(repo.path / file);
    repo.git(gitStageArguments(entry(file), true));
    check(entry(file).index == 'D' && !entry(file).changed(), "Staging a deletion did not remove its index entry");
    repo.git(gitStageArguments(entry(file), false));
    check(!entry(file).staged() && entry(file).worktree == 'D' && !fs::exists(repo.path / file), "Unstaging a deletion restored or lost the working tree deletion");

    repo.write(file, "initial\n"); repo.git({"mv", "--", file, renamed});
    const auto rename = entry(renamed);
    check(rename.index == 'R' && rename.previousPath == file, "The staging fixture was not a rename");
    repo.git(gitStageArguments(rename, false));
    repo.git({"diff", "--cached", "--exit-code"});
    check(!entry(file).staged() && entry(file).worktree == 'D' && entry(renamed).index == '?' &&
        !fs::exists(repo.path / file) && repo.readFile(renamed) == "initial\n", "Unstaging a rename left its source staged or changed working files");
}
void repositoryHistory() {
    Repository repo;
    repo.git({"init", "--quiet", "--template=", "--initial-branch=main"});
    repo.git({"config", "user.name", "Poppy Test"}); repo.git({"config", "user.email", "poppy@example.invalid"});
    repo.git({"config", "core.autocrlf", "false"}); repo.git({"config", "commit.gpgsign", "false"});
    repo.write("src/日本語.txt", "initial\n"); repo.write("delete.txt", "gone\n");
    repo.git({"add", "."}); repo.git({"commit", "-qm", "root 日本語"});
    const auto root = repo.hash();
    const auto rootFiles = parseGitCommitFiles(repo.git(gitCommitFilesArguments(root)));
    check(rootFiles.size() == 2 && rootFiles[0].state == 'A', "The root commit did not expose added files");
    for (size_t i = 1; i < 102; ++i) repo.git({"commit", "--allow-empty", "-qm", "commit " + std::to_string(i)});
    const auto tip = repo.hash();
    const auto first = parseGitCommits(repo.git(gitLogArguments(tip, 0)));
    const auto second = parseGitCommits(repo.git(gitLogArguments(tip, 50)));
    const auto last = parseGitCommits(repo.git(gitLogArguments(tip, 100)));
    check(first.size() == 51 && second.size() == 51 && last.size() == 2 && first[0].subject == "commit 101" && last.back().hash == root,
        "Commit pagination did not retrieve 50 entries plus a lookahead");
    check(first[50].hash == second[0].hash && second[50].hash == last[0].hash, "Pagination skipped or repeated a displayed commit");
    std::set<std::string> hashes;
    for (size_t i = 0; i < 50; ++i) { hashes.insert(first[i].hash); hashes.insert(second[i].hash); }
    for (const auto& commit : last) hashes.insert(commit.hash);
    check(hashes.size() == 102, "Pagination lost commits across three pages");
    repo.write("src/日本語.txt", "staged\n"); repo.git({"add", "."}); repo.write("src/日本語.txt", "working\n"); repo.write("new.txt", "untracked\n");
    const auto status = parseGitStatus(repo.git({"status", "--porcelain=v1", "-z", "--untracked-files=all"}));
    check(status.size() == 2 && std::any_of(status.begin(), status.end(), [](const GitStatusEntry& entry) { return entry.staged() && entry.changed(); }), "Real status did not classify partial staging");
    const auto staged = repo.git(gitDiffArguments(GitDiffKind::Staged, {}, {}));
    const auto working = repo.git(gitDiffArguments(GitDiffKind::Changes, {}, {}));
    check(staged.find("+staged") != staged.npos && staged.find("+working") == staged.npos && working.find("-staged") != working.npos && working.find("+working") != working.npos,
        "Staged and working tree comparisons used the wrong baselines");
    repo.git({"add", "."}); repo.git({"commit", "-qm", "update"});
    repo.git({"mv", "src/日本語.txt", "src/renamed.txt"}); repo.git({"rm", "delete.txt"}); repo.git({"commit", "-qm", "rename and delete"});
    const auto deleted = repo.hash();
    const auto deletedFiles = parseGitCommitFiles(repo.git(gitCommitFilesArguments(deleted)));
    check(deletedFiles.size() == 3 && std::count_if(deletedFiles.begin(), deletedFiles.end(), [](const GitCommitFile& file) { return file.state == 'D'; }) == 2,
        "Renames and deleted files were omitted from commit changes");
    const auto historic = repo.git(gitDiffArguments(GitDiffKind::Commit, root, "src/日本語.txt"));
    check(historic.find("+initial") != historic.npos && historic.find("+working") == historic.npos, "Historical diffs read the current file");
    repo.git({"checkout", "-qb", "topic"}); repo.write("topic.txt", "topic\n"); repo.git({"add", "."}); repo.git({"commit", "-qm", "topic"});
    repo.git({"checkout", "-q", "main"}); repo.write("main.txt", "main\n"); repo.git({"add", "."}); repo.git({"commit", "-qm", "main"});
    repo.git({"merge", "--no-ff", "--no-edit", "topic"});
    const auto mergeFiles = parseGitCommitFiles(repo.git(gitCommitFilesArguments(repo.hash())));
    check(mergeFiles.size() == 1 && mergeFiles[0].path == "topic.txt", "Merge commits must show changes against the first parent");
    check(repo.git(gitDiffArguments(GitDiffKind::Commit, repo.hash(), {})).find("+topic") != std::string::npos, "Merge commit patch is empty");
    const auto refs = parseGitBranches(repo.git({"for-each-ref", "--sort=refname", "--format=%(refname)%00%(refname:short)%00%(objectname)%00%(HEAD)", "refs/heads", "refs/remotes"}));
    check(refs.size() == 2 && refs[0].current && refs[0].name == "main", "Real branch listing lost its current branch");
    repo.write("literal[1].txt", "one\n"); repo.write("literal1.txt", "other\n"); repo.git({"add", "."}); repo.git({"commit", "-qm", "literal paths"});
    repo.write("literal[1].txt", "changed\n"); repo.write("literal1.txt", "wrong file\n");
    const auto literal = repo.git(gitDiffArguments(GitDiffKind::Changes, {}, "literal[1].txt"));
    check(literal.find("+changed") != literal.npos && literal.find("wrong file") == literal.npos, "A literal filename was interpreted as a pathspec pattern");
}
#endif
}
int main() {
    try {
        statusGroups(); records(); patchLists();
#ifndef _WIN32
        repositoryStaging(); repositoryHistory();
#endif
        std::cout << "Git history tests passed.\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
