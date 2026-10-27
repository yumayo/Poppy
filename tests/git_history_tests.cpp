#include "git_history.h"
#include "git_repositories.h"
#include "diff.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>
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
void repositoryOwnership() {
    const auto app = ResourceId::wsl("Ubuntu", "/home/user/app");
    const auto client = app / std::string_view("app-client"), server = app / std::string_view("app-server");
    const auto nested = client / std::string_view("packages/日本語");
    const std::vector<ResourceId> roots{server, app, nested, client};
    check(gitRepositoryFor(roots, client / std::string_view("same.txt")) == client, "A sibling file belongs to the wrong repository");
    check(gitRepositoryFor(roots, nested / std::string_view("same.txt")) == nested, "A nested file belongs to the outer repository");
    check(gitRepositoryFor({client}, app / std::string_view("app-client-other/same.txt")).empty(), "A path prefix was mistaken for repository membership");
    check(gitRepositoryFor(roots, ResourceId::wsl("Debian", "/home/user/app/app-client/same.txt")).empty(), "Repository lookup crossed distributions");
    check(gitRepositoryFor({client}, app / std::string_view("App-client/same.txt")).empty(), "Repository lookup folded a WSL path's case");
    check(gitRepositoryFor(roots, server) == server, "All Changes selected the wrong repository");
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
    void discardAll() const {
        const auto entries = parseGitStatus(git({"status", "--porcelain=v1", "-z", "--untracked-files=all"}));
        for (const auto& command : gitDiscardAllArguments(entries)) git(command);
    }
    void stashUnstaged() const {
        const auto args = gitStashUnstagedArguments(parseGitStatus(git({"status", "--porcelain=v1", "-z", "--untracked-files=all"})));
        if (!args.empty()) git(args);
    }
};
void multipleRepositories() {
    Repository fixture;
    const auto app = fixture.path / "app", client = app / "app-client", server = app / "app-server";
    fs::create_directories(client); fs::create_directories(server);
    check(scanGitRepositories(app).roots.empty(), "A non-repository was detected as Git");
    auto git = [&](const fs::path& directory, std::vector<std::string> args) {
        if (!args.empty() && args.front() == "git.exe") args.erase(args.begin());
        args.insert(args.begin(), {"-C", directory.string()}); return fixture.git(std::move(args));
    };
    git(fixture.path, {"init", "--quiet", "--template=", "--initial-branch=main"});
    fixture.write("unrelated/file.txt", "outside the opened folder\n");
    check(scanGitRepositories(app).roots.empty(), "Discovery included an ancestor repository");
    for (const auto& directory : {client, server}) {
        git(directory, {"init", "--quiet", "--template=", "--initial-branch=main"});
        git(directory, {"config", "user.name", "Poppy Test"}); git(directory, {"config", "user.email", "poppy@example.invalid"});
        git(directory, {"config", "commit.gpgsign", "false"});
        std::ofstream(directory / "same.txt") << "initial\n";
        git(directory, {"add", "."}); git(directory, {"commit", "-qm", directory.filename().string()});
    }
    std::ofstream(client / "same.txt") << "client change\n";
    std::ofstream(server / "same.txt") << "server change\n";
    const auto scan = scanGitRepositories(app);
    check(scan.roots.size() == 2 && scan.error.empty() && !scan.truncated, "Sibling repositories were not discovered below a non-Git parent");
    check(scanGitRepositories(client).roots == std::vector<ResourceId>{ResourceId(client)}, "Discovery missed the opened repository itself");
    const auto owner = gitRepositoryFor(scan.roots, ResourceId(client / "same.txt"));
    check(owner == ResourceId(client), "Discovery lost the owner of an identically named file");
    const auto patch = git(owner.local(), gitDiffArguments(GitDiffKind::Changes, {}, "same.txt"));
    check(patch.find("+client change") != patch.npos && patch.find("server change") == patch.npos, "Diff mixed sibling repositories");
    git(owner.local(), gitStageArguments({"same.txt", {}, ' ', 'M'}, true));
    check(parseGitStatus(git(client, {"status", "--porcelain=v1", "-z"}))[0].staged() &&
          !parseGitStatus(git(server, {"status", "--porcelain=v1", "-z"}))[0].staged(), "Staging crossed repository boundaries");
    fixture.write("app/app-client/new 日本語.txt", "client addition\n");
    git(client, gitStageAllArguments());
    const auto clientStatus = parseGitStatus(git(client, {"status", "--porcelain=v1", "-z"}));
    check(clientStatus.size() == 2 && std::all_of(clientStatus.begin(), clientStatus.end(), [](const GitStatusEntry& entry) { return entry.staged() && !entry.changed(); }) &&
          !parseGitStatus(git(server, {"status", "--porcelain=v1", "-z"}))[0].staged(), "Stage All Changes missed client changes or staged a sibling repository");
    git(server, gitStageAllArguments());
    const auto serverIndex = git(server, {"ls-files", "--stage", "-z"});
    git(client, gitUnstageAllArguments());
    git(client, {"diff", "--cached", "--quiet"});
    check(git(server, {"ls-files", "--stage", "-z"}) == serverIndex &&
          parseGitStatus(git(server, {"status", "--porcelain=v1", "-z"}))[0].staged() &&
          fixture.readFile("app/app-client/same.txt") == "client change\n" && fixture.readFile("app/app-client/new 日本語.txt") == "client addition\n",
          "Unstage All Changes changed a sibling index or the selected repository's working files");
    fixture.write("app/app-server/same.txt", "server working change\n");
    for (const auto& command : gitDiscardAllArguments(parseGitStatus(git(client, {"status", "--porcelain=v1", "-z", "--untracked-files=all"})))) git(client, command);
    check(fixture.readFile("app/app-client/same.txt") == "initial\n" && !fs::exists(client / "new 日本語.txt") &&
          fixture.readFile("app/app-server/same.txt") == "server working change\n" && git(server, {"ls-files", "--stage", "-z"}) == serverIndex,
          "Discard All Changes crossed repository boundaries or changed a sibling index");
    fixture.write("app/app-client/same.txt", "stash client\n");
    git(client, gitStashUnstagedArguments(parseGitStatus(git(client, {"status", "--porcelain=v1", "-z"}))));
    check(fixture.readFile("app/app-client/same.txt") == "initial\n" &&
          fixture.readFile("app/app-server/same.txt") == "server working change\n" && git(server, {"ls-files", "--stage", "-z"}) == serverIndex &&
          git(server, {"stash", "list"}).empty(), "Stash Unstaged Changes affected a sibling repository");
    fixture.git({"diff", "--cached", "--quiet"});
    const auto worktree = app / "worktree 日本語";
    git(client, {"worktree", "add", "--quiet", "--detach", worktree.string()});
    fs::create_directories(client / ".git" / "trap" / ".git");
    fs::create_directory_symlink(app, client / "loop");
    check(scanGitRepositories(app).roots.size() == 3, "Discovery entered .git or a directory link, or missed a gitfile worktree");
    fs::create_directories(client / "nested"); git(client / "nested", {"init", "--quiet", "--template=", "--initial-branch=main"});
    check(scanGitRepositories(app).roots.size() == 4, "Discovery stopped at an outer repository");
    check(scanGitRepositories(app, [] { return true; }).roots.empty(), "Repository scan ignored cancellation");
    check(scanGitRepositories(app, {}, {}, 1).truncated && scanGitRepositories(app, {}, {}, 50000, 0).truncated, "Repository scan did not report its limits");
}
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
void repositoryStageAll() {
    Repository repo;
    repo.git({"init", "--quiet", "--template=", "--initial-branch=main"});
    repo.git({"config", "user.name", "Poppy Test"}); repo.git({"config", "user.email", "poppy@example.invalid"});
    repo.git({"config", "core.autocrlf", "false"}); repo.git({"config", "commit.gpgsign", "false"});
    repo.git({"config", "core.excludesFile", "/dev/null"});
    repo.write(".gitignore", "ignored.txt\n"); repo.write("ignored.txt", "ignored\n");
    for (const auto& path : {"src/日本語.txt", "deleted.txt", "rename source.txt", "staged only.txt"}) repo.write(path, "initial\n");
    repo.git(gitStageAllArguments());
    auto entries = parseGitStatus(repo.git({"status", "--porcelain=v1", "-z", "--untracked-files=all"}));
    check(entries.size() == 5 && std::all_of(entries.begin(), entries.end(), [](const GitStatusEntry& entry) { return entry.index == 'A' && !entry.changed(); }),
        "Stage All Changes before the first commit missed files or included ignored files");
    repo.git({"commit", "-qm", "initial"});
    repo.write("src/日本語.txt", "staged\n"); repo.git({"add", "--", "src/日本語.txt"}); repo.write("src/日本語.txt", "working\n");
    repo.write("staged only.txt", "already staged\n"); repo.git({"add", "--", "staged only.txt"});
    fs::remove(repo.path / "deleted.txt"); fs::rename(repo.path / "rename source.txt", repo.path / "renamed 日本語.txt");
    repo.write("nested/new [1].txt", "untracked\n");
    const auto index = repo.git({"ls-files", "--stage", "-z"});
    repo.write(".git/index.lock", "busy\n");
    bool failed = false;
    try { repo.git(gitStageAllArguments()); } catch (const std::runtime_error&) { failed = true; }
    fs::remove(repo.path / ".git/index.lock");
    check(failed && repo.git({"ls-files", "--stage", "-z"}) == index && repo.readFile("src/日本語.txt") == "working\n",
        "Stage All Changes did not preserve the index and working files when the index was locked");
    repo.git(gitStageAllArguments());
    entries = parseGitStatus(repo.git({"status", "--porcelain=v1", "-z", "--untracked-files=all"}));
    check(entries.size() == 5 && std::all_of(entries.begin(), entries.end(), [](const GitStatusEntry& entry) { return entry.staged() && !entry.changed(); }),
        "Stage All Changes missed a modification, deletion, rename or untracked file");
    check(repo.git({"show", ":src/日本語.txt"}) == "working\n" && repo.git({"show", ":nested/new [1].txt"}) == "untracked\n" &&
          repo.git({"show", ":staged only.txt"}) == "already staged\n", "Stage All Changes did not include the latest working text or preserve already staged text");
    check(repo.readFile("src/日本語.txt") == "working\n" && repo.readFile("renamed 日本語.txt") == "initial\n" &&
          repo.readFile("nested/new [1].txt") == "untracked\n" && repo.readFile("ignored.txt") == "ignored\n" &&
          !fs::exists(repo.path / "deleted.txt") && !fs::exists(repo.path / "rename source.txt"), "Stage All Changes modified working files");
    repo.git({"diff", "--quiet"});
    repo.git(gitStageAllArguments());
}
void repositoryUnstageAll() {
    Repository repo;
    repo.git({"init", "--quiet", "--template=", "--initial-branch=main"});
    repo.git({"config", "user.name", "Poppy Test"}); repo.git({"config", "user.email", "poppy@example.invalid"});
    repo.git({"config", "core.autocrlf", "false"}); repo.git({"config", "commit.gpgsign", "false"});
    repo.git({"config", "core.excludesFile", "/dev/null"});
    repo.write(".gitignore", "ignored.txt\n"); repo.write("ignored.txt", "ignored\n");
    for (const auto& path : {"src/日本語 [1].txt", "deleted.txt", "rename source.txt", "staged only.txt"}) repo.write(path, "initial\n");
    repo.git(gitStageAllArguments());
    const auto branch = repo.git({"symbolic-ref", "HEAD"});
    repo.git(gitUnstageAllArguments());
    auto entries = parseGitStatus(repo.git({"status", "--porcelain=v1", "-z", "--untracked-files=all"}));
    check(repo.git({"ls-files", "--stage", "-z"}).empty() && entries.size() == 5 &&
          std::all_of(entries.begin(), entries.end(), [](const GitStatusEntry& entry) { return entry.index == '?'; }) &&
          repo.git({"symbolic-ref", "HEAD"}) == branch && repo.readFile("src/日本語 [1].txt") == "initial\n",
          "Unstage All Changes before the first commit lost files or left entries staged");
    repo.git(gitStageAllArguments()); repo.git({"commit", "-qm", "initial"});
    const auto head = repo.hash(), committedIndex = repo.git({"ls-files", "--stage", "-z"});
    repo.write("src/日本語 [1].txt", "staged\n"); repo.git({"add", "--", "src/日本語 [1].txt"});
    repo.write("src/日本語 [1].txt", "working\n");
    repo.write("staged only.txt", "already staged\n"); repo.git({"add", "--", "staged only.txt"});
    fs::remove(repo.path / "deleted.txt"); repo.git({"add", "--", "deleted.txt"});
    repo.git({"mv", "--", "rename source.txt", "renamed 日本語.txt"});
    repo.write("nested/new [2].txt", "addition\n"); repo.git({"add", "--", "nested/new [2].txt"});
    repo.write("untracked.txt", "untracked\n");
    const auto workingFilesUnchanged = [&] {
        return repo.readFile("src/日本語 [1].txt") == "working\n" && repo.readFile("staged only.txt") == "already staged\n" &&
            repo.readFile("renamed 日本語.txt") == "initial\n" && repo.readFile("nested/new [2].txt") == "addition\n" &&
            repo.readFile("untracked.txt") == "untracked\n" && repo.readFile("ignored.txt") == "ignored\n" &&
            !fs::exists(repo.path / "deleted.txt") && !fs::exists(repo.path / "rename source.txt");
    };
    const auto index = repo.git({"ls-files", "--stage", "-z"});
    repo.write(".git/index.lock", "busy\n");
    bool failed = false;
    try { repo.git(gitUnstageAllArguments()); } catch (const std::runtime_error&) { failed = true; }
    fs::remove(repo.path / ".git/index.lock");
    check(failed && repo.git({"ls-files", "--stage", "-z"}) == index && workingFilesUnchanged(),
        "Unstage All Changes did not preserve the index and working files when the index was locked");
    repo.git(gitUnstageAllArguments());
    entries = parseGitStatus(repo.git({"status", "--porcelain=v1", "-z", "--untracked-files=all"}));
    check(repo.git({"ls-files", "--stage", "-z"}) == committedIndex && entries.size() == 7 &&
          std::all_of(entries.begin(), entries.end(), [](const GitStatusEntry& entry) { return !entry.staged() && entry.changed(); }) &&
          repo.hash() == head && repo.git({"symbolic-ref", "HEAD"}) == branch && workingFilesUnchanged(),
          "Unstage All Changes missed an index entry, moved HEAD or changed working files");
    repo.git({"diff", "--cached", "--quiet"});
    repo.git(gitUnstageAllArguments());
    check(repo.git({"ls-files", "--stage", "-z"}) == committedIndex && workingFilesUnchanged(),
        "Unstage All Changes changed files when the index was already clean");
}
void repositoryDiscardAll() {
    Repository repo;
    repo.git({"init", "--quiet", "--template=", "--initial-branch=main"});
    repo.git({"config", "user.name", "Poppy Test"}); repo.git({"config", "user.email", "poppy@example.invalid"});
    repo.git({"config", "core.autocrlf", "false"}); repo.git({"config", "commit.gpgsign", "false"});
    repo.git({"config", "core.excludesFile", "/dev/null"});
    // No HEAD or index exists. Empty directories and untracked files are removed.
    repo.write("new 日本語 [1].txt", "untracked\n"); fs::create_directory(repo.path / "empty");
    repo.discardAll();
    check(!fs::exists(repo.path / "new 日本語 [1].txt") && !fs::exists(repo.path / "empty"),
          "Discard All Changes failed before the first index or commit");
    repo.write(".gitignore", "ignored.txt\nignored-dir/\n");
    repo.write("ignored.txt", "keep\n"); repo.write("ignored-dir/file", "keep\n");
    repo.write("src/日本語 [1].txt", "staged before first commit\n");
    repo.git({"add", "."});
    const auto initialIndex = repo.git({"ls-files", "--stage", "-z"});
    repo.write("src/日本語 [1].txt", "unstaged\n"); repo.write("nested/untracked.txt", "remove\n");
    repo.discardAll();
    check(repo.readFile("src/日本語 [1].txt") == "staged before first commit\n" && !fs::exists(repo.path / "nested") &&
          repo.git({"ls-files", "--stage", "-z"}) == initialIndex && repo.readFile("ignored.txt") == "keep\n",
          "Discard All Changes lost staged or ignored content before the first commit");
    for (const auto& file : {"deleted.txt", "rename source.txt", "staged deletion.txt", "staged rename.txt"}) repo.write(file, "initial\n");
    repo.git({"add", "."}); repo.git({"commit", "-qm", "baseline"});
    const auto head = repo.hash();
    repo.write("src/日本語 [1].txt", "partially staged\n"); repo.git({"add", "."});
    repo.write("src/日本語 [1].txt", "working\n");
    fs::remove(repo.path / "deleted.txt");
    fs::rename(repo.path / "rename source.txt", repo.path / "renamed 日本語.txt");
    repo.git({"rm", "--", "staged deletion.txt"});
    repo.git({"mv", "--", "staged rename.txt", "staged renamed.txt"});
    repo.write("staged renamed.txt", "working rename\n");
    repo.write("added.txt", "staged addition\n"); repo.git({"add", "--", "added.txt"});
    fs::remove(repo.path / "added.txt");
    repo.write("new/quoted' [2]\nfile.txt", "untracked\n");
    // Both forms of nested repositories and an external symlink target survive.
    fs::create_directories(repo.path / "nested-repo");
    repo.git({"-C", (repo.path / "nested-repo").string(), "init", "-q", "--template="});
    repo.write("nested-repo/keep.txt", "nested\n");
    repo.git({"worktree", "add", "--quiet", "--detach", (repo.path / "nested-worktree").string()});
    repo.write("nested-worktree/src/日本語 [1].txt", "nested worktree\n");
    Repository outside; outside.write("keep.txt", "outside\n");
    fs::create_directory_symlink(outside.path, repo.path / "untracked-link");
    const auto index = repo.git({"ls-files", "--stage", "-z"});
    repo.discardAll();
    check(repo.hash() == head && repo.git({"ls-files", "--stage", "-z"}) == index &&
          repo.readFile("src/日本語 [1].txt") == "partially staged\n" && repo.readFile("added.txt") == "staged addition\n" &&
          repo.readFile("deleted.txt") == "initial\n" && repo.readFile("rename source.txt") == "initial\n" &&
          repo.readFile("staged renamed.txt") == "initial\n" && !fs::exists(repo.path / "staged rename.txt") &&
          !fs::exists(repo.path / "staged deletion.txt") && !fs::exists(repo.path / "renamed 日本語.txt") && !fs::exists(repo.path / "new") &&
          repo.readFile("ignored.txt") == "keep\n" && repo.readFile("ignored-dir/file") == "keep\n" &&
          repo.readFile("nested-repo/keep.txt") == "nested\n" && repo.readFile("nested-worktree/src/日本語 [1].txt") == "nested worktree\n" &&
          !fs::exists(repo.path / "untracked-link") && outside.readFile("keep.txt") == "outside\n",
          "Discard All Changes failed to restore the index or protect staged, ignored, nested or external content");
    repo.git({"diff", "--quiet"});
    repo.discardAll(); // Repeating a clean operation keeps staged changes intact.
    check(repo.git({"ls-files", "--stage", "-z"}) == index, "Repeated discard changed the index");
    // Reject a conflicted index before restoring unrelated files or cleaning.
    repo.git({"add", "--all", "--", "src/日本語 [1].txt", "added.txt", "staged renamed.txt"});
    repo.git({"commit", "-qm", "staged changes"});
    repo.git({"checkout", "-qb", "topic"}); repo.write("deleted.txt", "topic\n"); repo.git({"add", "deleted.txt"}); repo.git({"commit", "-qm", "topic"});
    repo.git({"checkout", "-q", "main"}); repo.write("deleted.txt", "main\n"); repo.git({"add", "deleted.txt"}); repo.git({"commit", "-qm", "main"});
    try { repo.git({"merge", "topic"}); } catch (const std::runtime_error&) {}
    repo.write("src/日本語 [1].txt", "keep working\n"); repo.write("keep untracked.txt", "keep\n");
    const auto conflictedIndex = repo.git({"ls-files", "--stage", "-z"}), conflict = repo.readFile("deleted.txt");
    bool failed = false;
    try { repo.discardAll(); } catch (const std::invalid_argument&) { failed = true; }
    check(failed && repo.git({"ls-files", "--stage", "-z"}) == conflictedIndex && repo.readFile("deleted.txt") == conflict &&
          repo.readFile("src/日本語 [1].txt") == "keep working\n" && repo.readFile("keep untracked.txt") == "keep\n",
          "Discard All Changes modified files before rejecting merge conflicts");
}
void repositoryStashUnstaged() {
    Repository repo;
    repo.git({"init", "--quiet", "--template=", "--initial-branch=main"});
    repo.git({"config", "user.name", "Poppy Test"}); repo.git({"config", "user.email", "poppy@example.invalid"});
    repo.git({"config", "core.autocrlf", "false"}); repo.git({"config", "commit.gpgsign", "false"});
    repo.git({"config", "core.excludesFile", "/dev/null"});
    repo.write("partial 日本語 [1].txt", "initial\n");
    bool failed = false;
    try { repo.stashUnstaged(); } catch (const std::runtime_error&) { failed = true; }
    check(failed && repo.readFile("partial 日本語 [1].txt") == "initial\n" && repo.git({"stash", "list"}).empty(),
          "Stashing before the first commit did not fail without changing files");
    repo.write(".gitignore", "ignored.txt\n");
    for (const auto& file : {"deleted.txt", "rename source.txt", "staged only.txt", "staged deletion.txt", "staged rename.txt"}) repo.write(file, "initial\n");
    repo.git({"add", "."}); repo.git({"commit", "-qm", "baseline"});
    const auto head = repo.hash();
    repo.write("partial 日本語 [1].txt", "staged\n"); repo.write("staged only.txt", "staged only\n");
    repo.write("added.txt", "staged addition\n"); repo.git({"add", "."});
    repo.git({"rm", "--", "staged deletion.txt"}); repo.git({"mv", "--", "staged rename.txt", "staged renamed.txt"});
    const auto index = repo.git({"ls-files", "--stage", "-z"});
    // A stale Changes group must not create a stash containing only the index.
    repo.stashUnstaged();
    check(repo.git({"stash", "list"}).empty(), "Staged-only changes created an unstaged stash");
    repo.write("partial 日本語 [1].txt", "working\n"); repo.write("staged renamed.txt", "working rename\n");
    fs::remove(repo.path / "added.txt"); fs::remove(repo.path / "deleted.txt");
    fs::rename(repo.path / "rename source.txt", repo.path / "renamed 日本語.txt");
    const std::string untracked = "new/quoted' [2]\nfile.txt", binary("\0\1\2", 3);
    repo.write(untracked, "untracked\n"); repo.write("binary.dat", binary); repo.write("ignored.txt", "keep\n");
    failed = false;
    try { repo.stashUnstaged(); } catch (const std::invalid_argument&) { failed = true; }
    check(failed && !fs::exists(repo.path / "added.txt") && repo.git({"ls-files", "--stage", "-z"}) == index &&
          repo.git({"stash", "list"}).empty() && repo.readFile(untracked) == "untracked\n",
          "An unsupported staged addition deletion changed files or created a stash");
    repo.write("added.txt", "working addition\n");
    repo.write(".git/index.lock", "locked\n"); failed = false;
    try { repo.stashUnstaged(); } catch (const std::runtime_error&) { failed = true; }
    fs::remove(repo.path / ".git/index.lock");
    check(failed && repo.git({"ls-files", "--stage", "-z"}) == index && repo.git({"stash", "list"}).empty() &&
          repo.readFile("partial 日本語 [1].txt") == "working\n" && repo.readFile(untracked) == "untracked\n",
          "Stashing with a locked index changed files or the stash list");
    repo.stashUnstaged();
    check(repo.hash() == head && repo.git({"ls-files", "--stage", "-z"}) == index &&
          repo.readFile("partial 日本語 [1].txt") == "staged\n" && repo.readFile("staged only.txt") == "staged only\n" &&
          repo.readFile("added.txt") == "staged addition\n" && repo.readFile("staged renamed.txt") == "initial\n" &&
          !fs::exists(repo.path / "staged deletion.txt") && !fs::exists(repo.path / "staged rename.txt") &&
          repo.readFile("deleted.txt") == "initial\n" && repo.readFile("rename source.txt") == "initial\n" &&
          !fs::exists(repo.path / "renamed 日本語.txt") && !fs::exists(repo.path / "new") && !fs::exists(repo.path / "binary.dat") &&
          repo.readFile("ignored.txt") == "keep\n", "Stash Unstaged Changes lost staged content or retained working changes");
    repo.git({"diff", "--quiet"});
    check(repo.git({"show", "stash@{0}:partial 日本語 [1].txt"}) == "working\n" &&
          repo.git({"show", "stash@{0}^2:partial 日本語 [1].txt"}) == "staged\n" &&
          repo.git({"show", "stash@{0}:added.txt"}) == "working addition\n" &&
          repo.git({"show", "stash@{0}^2:added.txt"}) == "staged addition\n" &&
          repo.git({"show", "stash@{0}^3:" + untracked}) == "untracked\n" && repo.git({"show", "stash@{0}^3:binary.dat"}) == binary,
          "The stash did not retain working, staged, untracked or binary content");
    const auto stash = repo.git({"rev-parse", "refs/stash"});
    repo.stashUnstaged();
    check(repo.git({"rev-parse", "refs/stash"}) == stash, "Repeating stash on a clean worktree created another entry");
    // Git's keep-index stash includes both snapshots and can restore the index.
    repo.git({"restore", "--source=HEAD", "--staged", "--worktree", "--", "."});
    repo.git({"stash", "apply", "--index"});
    check(repo.git({"ls-files", "--stage", "-z"}) == index, "Stash apply did not restore the index");
    check(repo.readFile("partial 日本語 [1].txt") == "working\n" && repo.readFile("staged renamed.txt") == "working rename\n",
          "Stash apply did not restore partially staged file text");
    check(repo.readFile("added.txt") == "working addition\n", "Stash apply did not restore a partially staged addition");
    check(!fs::exists(repo.path / "deleted.txt") && repo.readFile("renamed 日本語.txt") == "initial\n" &&
          repo.readFile(untracked) == "untracked\n" && repo.readFile("binary.dat") == binary,
          "Stash apply lost tracked, renamed or untracked content");
    // Conflict rejection must precede saving or cleaning unrelated files.
    repo.git({"restore", "--source=HEAD", "--staged", "--worktree", "--", "."});
    repo.git({"checkout", "-qb", "topic"}); repo.write("deleted.txt", "topic\n"); repo.git({"add", "deleted.txt"}); repo.git({"commit", "-qm", "topic"});
    repo.git({"checkout", "-q", "main"}); repo.write("deleted.txt", "main\n"); repo.git({"add", "deleted.txt"}); repo.git({"commit", "-qm", "main"});
    try { repo.git({"merge", "topic"}); } catch (const std::runtime_error&) {}
    const auto conflict = repo.readFile("deleted.txt"), conflictedIndex = repo.git({"ls-files", "--stage", "-z"});
    failed = false;
    try { repo.stashUnstaged(); } catch (const std::invalid_argument&) { failed = true; }
    check(failed && repo.readFile("deleted.txt") == conflict && repo.git({"ls-files", "--stage", "-z"}) == conflictedIndex &&
          repo.readFile(untracked) == "untracked\n" && repo.git({"rev-parse", "refs/stash"}) == stash,
          "Stashing with conflicts changed files, the index or an existing stash");
}
void repositorySyntax() {
    Repository repo;
    repo.git({"init", "--quiet", "--template=", "--initial-branch=main"});
    repo.git({"config", "user.name", "Poppy Test"}); repo.git({"config", "user.email", "poppy@example.invalid"});
    repo.git({"config", "core.autocrlf", "false"}); repo.git({"config", "commit.gpgsign", "false"});
    const std::vector<std::pair<std::string, Language>> files{
        {"src/日本語.CPP", Language::CLike}, {"src/with spaces.py", Language::Python},
        {"src/quoted\"name.py", Language::Python}, {"src/tab\tname.py", Language::Python},
        {"dir/CMakeLists.txt", Language::Config}};
    std::vector<std::string> add{"add", "--"};
    for (const auto& file : files) add.push_back(file.first);
    for (const auto& file : files) repo.write(file.first, "return 1\n");
    repo.git(add); repo.git({"commit", "-qm", "syntax baseline"});
    const auto revision = repo.hash();
    for (const auto& file : files) repo.write(file.first, "return 2\n");
    repo.git(add);
    for (const auto& file : files) repo.write(file.first, "return 3\n");
    for (const auto kind : {GitDiffKind::Changes, GitDiffKind::Staged, GitDiffKind::Commit}) {
        for (const auto& file : files) {
            DiffDocument diff;
            diff.patch.text = repo.git(gitDiffArguments(kind, revision, file.first));
            indexDiff(diff, true);
            size_t sources = 0;
            for (const auto& row : diff.rows) for (const auto source : {row.before, row.after}) {
                if (!source.number) continue;
                ++sources;
                check(source.language == file.second, "Real Git patch headers lost a language or a quoted path suffix");
                auto state = source.state; std::vector<Token> tokens;
                lexLine(diff.line(source), source.language, state, &tokens);
                check(!tokens.empty() && tokens.front().begin == 0 && tokens.front().style == Style::Keyword && tokens.back().style == Style::Number,
                    "Working, staged and historical diffs must highlight source tokens without patch prefixes");
            }
            check(sources == (kind == GitDiffKind::Commit ? 1 : 2), "Syntax fixture lost original or modified source lines");
        }
    }
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
        statusGroups(); records(); patchLists(); repositoryOwnership();
#ifndef _WIN32
        repositoryStaging(); repositoryStageAll(); repositoryUnstageAll(); repositoryDiscardAll(); repositoryStashUnstaged(); repositorySyntax(); repositoryHistory(); multipleRepositories();
#endif
        std::cout << "Git history tests passed.\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
