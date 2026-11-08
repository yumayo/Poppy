#include "session.h"
#include "command_line.h"
#include "editor_tabs.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void editorTabPathTests() {
    using namespace poppy;
    const auto root = ResourceId::wsl("Ubuntu", "/home/user/日本語 project", "user");
    EditorTab tab; tab.path = root / std::string_view("nested/repository/src/日本語 file.cpp");
    tab.gitRoot = root / std::string_view("nested/repository");
    for (const auto kind : {GitDiffKind::Automatic, GitDiffKind::Changes, GitDiffKind::Staged, GitDiffKind::Commit, GitDiffKind::Compare}) {
        tab.diffKind = kind; tab.diff = kind != GitDiffKind::Automatic;
        check(editorTabPathText(tab, root, false) == L"/home/user/日本語 project/nested/repository/src/日本語 file.cpp",
              "Copy Path must preserve the Linux path for file and Git tabs");
        check(editorTabPathText(tab, root, true) == L"nested/repository/src/日本語 file.cpp",
              "Copy Relative Path must use the project root rather than the Git root");
    }
    tab.path = root / std::string_view("file\\name.txt");
    check(editorTabPathText(tab, root, true) == L"file\\name.txt", "Copying a Linux path changed a literal backslash");
    check(!editorTabPathText(tab, ResourceId::wsl("Debian", "/project"), true), "Paths from different WSL connections cannot be relative");
    tab.allChanges = true;
    check(!editorTabPathText(tab, root, false) && !editorTabPathText(tab, root, true), "All Changes has no single file path to copy");
    tab = {};
    check(!editorTabPathText(tab, root, false) && !editorTabPathText(tab, root, true), "An empty tab has no path to copy");

    const auto localRoot = std::filesystem::temp_directory_path() / std::filesystem::u8path("日本語 project");
    auto relative = std::filesystem::u8path("nested/日本語 file.cpp");
    auto localPath = localRoot / relative;
    tab.path = localPath;
    check(editorTabPathText(tab, ResourceId(localRoot), false) == resourceWide(localPath.make_preferred().u8string()),
          "Copy Path must use the local absolute path and native separators");
    check(editorTabPathText(tab, ResourceId(localRoot), true) == resourceWide(relative.make_preferred().u8string()),
          "Copy Relative Path must preserve Unicode and native separators");
    check(!editorTabPathText(tab, {}, true), "A relative path requires a project root");
#ifdef _WIN32
    tab.path = std::filesystem::path(L"D:\\other\\file.txt");
    check(!editorTabPathText(tab, ResourceId(L"C:\\project"), true), "Different drives have no relative path");
#endif
}
void editorTabTests() {
    using namespace poppy;
    const auto target = [](const char* name, int group = 0, GitDiffKind kind = GitDiffKind::Automatic) {
        EditorTab tab; tab.path = ResourceId::wsl("Ubuntu", std::string("/project/") + name); tab.group = group;
        tab.diffKind = kind; tab.diff = kind != GitDiffKind::Automatic;
        return tab;
    };
    std::vector<EditorTab> tabs;
    auto index = openEditorPreview(tabs, target("first.cpp"));
    check(index == 0 && tabs.size() == 1 && tabs[0].preview, "New files must open as previews");
    tabs[0].fileView.top = 40; tabs[0].diffView.caret = 12; tabs[0].searchQuery = "needle";
    tabs[0].searchTarget = ContentMatch{};
    check(openEditorPreview(tabs, target("second.cpp")) == 0 && tabs.size() == 1,
          "Opening another file accumulated preview tabs");
    check(tabs[0].fileView.top == 0 && tabs[0].diffView.caret == 0 && !tabs[0].searchTarget && tabs[0].searchQuery.empty(),
          "A replacement inherited the previous file's view or search target");
    for (const auto kind : {GitDiffKind::Changes, GitDiffKind::Staged, GitDiffKind::Commit, GitDiffKind::Compare}) {
        auto diff = target("diff.cpp", 0, kind); diff.revision = kind == GitDiffKind::Commit ? "abc123" : "";
        openEditorPreview(tabs, diff);
        check(tabs.size() == 1 && tabs[0].preview && tabs[0].diffKind == kind,
              "Browsing Git diffs accumulated tabs or reused the wrong comparison");
        diff.allChanges = true; diff.path = ResourceId::wsl("Ubuntu", "/project"); diff.gitRoot = diff.path;
        openEditorPreview(tabs, diff);
        check(tabs.size() == 1 && tabs[0].allChanges && tabs[0].gitRoot == diff.path,
              "All Changes did not replace the preview");
    }
    tabs[0].preview = false; tabs[0].diffView.top = 25;
    const auto kept = tabs[0];
    check(openEditorPreview(tabs, target("third.cpp")) == 1 && tabs.size() == 2, "A permanent tab was replaced");
    check(openEditorPreview(tabs, kept) == 0 && !tabs[0].preview && tabs[0].diffView.top == 25 && tabs.size() == 2,
          "Reopening a permanent diff duplicated it or lost its state");
    auto otherRevision = kept; otherRevision.revision = "different";
    check(openEditorPreview(tabs, otherRevision) == 1 && tabs[0].revision != tabs[1].revision,
          "Different Git revisions were treated as the same tab");
    openEditorPreview(tabs, target("right.cpp", 1));
    openEditorPreview(tabs, target("left.cpp"));
    check(tabs.size() == 3 && tabs[2].path == target("right.cpp", 1).path, "The left preview replaced the right group");
    openEditorPreview(tabs, target("right-next.cpp", 1));
    check(tabs.size() == 3 && tabs[1].path == target("left.cpp").path, "The right preview replaced the left group");

    tabs.clear();
    openEditorPreview(tabs, target("edited.cpp"));
    auto buffer = std::make_shared<EditBuffer>(); buffer->document.text = "original\n";
    indexDocument(buffer->document); buffer->initialize("original\n"); tabs[0].buffer = buffer;
    buffer->replace(0, 0, "!"); keepModifiedEditorTabs(tabs);
    check(!tabs[0].preview, "Editing a preview did not keep it open");
    auto diff = target("edited.cpp", 0, GitDiffKind::Changes);
    index = openEditorPreview(tabs, diff);
    check(index == 1 && !tabs[1].preview && tabs[1].buffer == buffer && buffer->document.text == "!original\n",
          "Opening a diff lost a shared unsaved buffer");
    size_t anchor = 0, caret = 0; buffer->undo(anchor, caret); keepModifiedEditorTabs(tabs);
    check(!tabs[0].preview && !tabs[1].preview && !buffer->dirty(), "Undo made edited tabs replaceable again");
    openEditorPreview(tabs, target("saving.cpp"));
    auto saving = std::make_shared<EditBuffer>(); saving->document.text = "saved";
    indexDocument(saving->document); saving->initialize("saved"); saving->saving = true; tabs[2].buffer = saving;
    openEditorPreview(tabs, target("next.cpp"));
    check(tabs.size() == 4 && !tabs[2].preview && tabs[2].buffer == saving, "A pending save was replaced");
    tabs[3].pinned = true;
    openEditorPreview(tabs, target("last.cpp"));
    check(tabs.size() == 5 && !tabs[3].preview, "Pinning a preview did not protect it");

    // A commit's full file and patch have separate identities and view state.
    tabs.clear();
    auto historic = target("historic.cpp", 0, GitDiffKind::Commit); historic.revision = "abc123";
    openEditorPreview(tabs, historic); tabs[0].preview = false; tabs[0].diffView.top = 15;
    historic.diff = false;
    check(openEditorPreview(tabs, historic) == 1 && !tabs[1].diff && !tabs[1].buffer,
        "Open Revision reused a diff tab or acquired the working buffer");
    tabs[1].preview = false; tabs[1].fileView.top = 27;
    check(openEditorPreview(tabs, historic) == 1 && tabs[1].fileView.top == 27 && tabs[0].diffView.top == 15,
        "Reopening a revision lost its view state or changed the diff's position");
    ProjectSession revisions; saveEditorTabs(revisions, tabs, 1);
    check(revisions.tabs.size() == 2 && revisions.tabs[0].diff && !revisions.tabs[1].diff &&
        revisions.tabs[1].diffKind == GitDiffKind::Commit && revisions.tabs[1].revision == "abc123",
        "Saving tabs lost the distinction between a historical file and its diff");

    // File comparisons have distinct identities and never own editable buffers.
    tabs.clear();
    auto fileComparison = target("active.cpp"); fileComparison.diff = true;
    fileComparison.comparePath = target("left.cpp").path;
    openEditorPreview(tabs, target("active.cpp")); tabs[0].buffer = buffer; tabs[0].preview = false;
    check(openEditorPreview(tabs, fileComparison) == 1 && !tabs[1].buffer,
          "A read-only file comparison reused the editable tab or its buffer");
    tabs[1].preview = false; tabs[1].diffView.top = 12;
    fileComparison.comparePath = target("other.cpp").path;
    check(openEditorPreview(tabs, fileComparison) == 2, "Different file comparison targets reused the same tab");
    fileComparison.comparePath = target("left.cpp").path;
    check(openEditorPreview(tabs, fileComparison) == 1 && tabs[1].diffView.top == 12,
          "Reopening a file comparison lost its view state");
    ProjectSession comparisons; saveEditorTabs(comparisons, tabs, 1);
    check(comparisons.tabs.size() == 2 && comparisons.tabs[1].comparePath == fileComparison.comparePath.u8string(),
          "Saving permanent file comparisons lost the comparison path");

    // Clean file/diff transitions also share Undo history before replacement.
    tabs.clear(); openEditorPreview(tabs, target("clean.cpp")); tabs[0].buffer = buffer;
    openEditorPreview(tabs, target("clean.cpp", 0, GitDiffKind::Changes));
    check(tabs.size() == 1 && tabs[0].buffer == buffer && tabs[0].preview, "Replacing a clean file with its diff lost the working buffer");
    openEditorPreview(tabs, target("Clean.cpp"));
    check(tabs[0].path == target("Clean.cpp").path, "WSL preview paths lost their case");

    ProjectSession session;
    saveEditorTabs(session, tabs, 0);
    check(session.tabs.empty() && session.activeTab == 0, "A preview was persisted");
    tabs.clear();
    openEditorPreview(tabs, target("temporary-left.cpp"));
    openEditorPreview(tabs, target("permanent-right.cpp", 1));
    tabs[1].preview = false; tabs[1].displayed = true; tabs[1].fileView.top = 30;
    openEditorPreview(tabs, target("temporary-right.cpp", 1));
    tabs[1].displayed = false; tabs[2].displayed = true;
    saveEditorTabs(session, tabs, 2);
    check(session.tabs.size() == 1 && session.activeTab == 0 && session.tabs[0].group == 0 &&
          session.tabs[0].displayed && session.tabs[0].fileView.top == 30,
          "Filtering previews did not repair a split group, selection or view state");
    tabs[0].preview = false; tabs[0].displayed = true;
    saveEditorTabs(session, tabs, 1);
    check(session.tabs.size() == 2 && session.activeTab == 1 && session.tabs[1].group == 1 && session.tabs[1].displayed,
          "Filtering a later preview lost the active permanent tab");
    tabs[0].buffer = saving; tabs[0].preview = true;
    saveEditorTabs(session, tabs, 0);
    check(session.tabs.size() == 2, "Snapshot filtering dropped a saving buffer");
}
void droppedFileTests() {
    using namespace poppy;
    const ResourceId root = std::filesystem::temp_directory_path() / "project";
    const ResourceId first = root.parent_path() / std::filesystem::u8path("外部 CSV/first.csv");
    const ResourceId second = root.parent_path() / std::string_view("second.csv");
    std::vector<EditorTab> tabs(1);
    tabs[0].path = root / std::string_view("preview.cpp"); tabs[0].preview = true;
    const auto index = openEditorDroppedFile(tabs, first, 2);
    check(index == 1 && tabs.size() == 2 && !tabs[index].preview && tabs[index].group == 2 && tabs[index].path == first &&
          tabs[0].preview, "A file drop replaced the existing preview or ignored its destination pane");
    tabs[index].fileView.top = 41; tabs[index].pinned = true;
    auto buffer = std::make_shared<EditBuffer>(); buffer->document.text = "name,value\n";
    indexDocument(buffer->document); buffer->initialize("name,value\n"); buffer->replace(0, 0, "!"); tabs[index].buffer = buffer;
    openEditorDroppedFile(tabs, second, 2);
    check(tabs.size() == 3 && tabs[1].path == first && tabs[2].path == second && !tabs[2].preview,
          "Dropping multiple files discarded an earlier file");
    check(openEditorDroppedFile(tabs, first, 0) == index && tabs.size() == 3 && tabs[index].group == 2 &&
          tabs[index].fileView.top == 41 && tabs[index].pinned && tabs[index].buffer == buffer && buffer->dirty(),
          "Dropping an open file duplicated or moved it, or lost unsaved text or view state");
    tabs[2].displayed = true;
    ProjectSession session; saveEditorTabs(session, tabs, 2);
    check(session.tabs.size() == 2 && restoreEditorTabPath(session.tabs[0].path, root) == first &&
          restoreEditorTabPath(session.tabs[1].path, root) == second,
          "Dropped external files did not survive saving and restoring tabs");

    // A drop opens the working file even when its automatic diff is selected.
    tabs.clear(); EditorTab diff; diff.path = first; diff.diff = true; diff.preview = true; diff.buffer = buffer;
    tabs.push_back(diff);
    check(openEditorDroppedFile(tabs, first, 0) == 0 && !tabs[0].diff && !tabs[0].preview && tabs[0].buffer == buffer,
          "Dropping a file with an open automatic diff did not reveal its working text");
    tabs[0].diffKind = GitDiffKind::Changes;
    check(openEditorDroppedFile(tabs, first, 1) == 1 && tabs[1].buffer == buffer && !tabs[1].diff,
          "Opening a dropped file from a Git diff lost the shared working buffer");

    const auto wsl = ResourceId::wsl("Ubuntu", "/home/user/project", "user");
    const auto legacy = wsl.at("/home/user/日本語/file.csv");
    check(restoreEditorTabPath(legacy.u8string(), wsl) == legacy &&
          restoreEditorTabPath(legacy.serialized(), wsl) == legacy,
          "Restoring dropped files changed existing WSL session paths or their user");
    tabs.clear();
    const auto upper = wsl.at("/outside/A.csv"), lower = wsl.at("/outside/a.csv");
    openEditorDroppedFile(tabs, upper, 0); openEditorDroppedFile(tabs, lower, 0);
    check(tabs.size() == 2, "Dropped WSL files lost their case-sensitive identity");
#ifdef _WIN32
    const ResourceId local = L"D:\\外部 CSV\\file.csv";
    check(restoreEditorTabPath(local.u8string(), wsl) == local &&
          restoreEditorTabPath("\\\\server\\share\\file.csv", wsl) == ResourceId(L"\\\\server\\share\\file.csv"),
          "A Windows drive or UNC path was restored as a Linux path in a WSL project");
    tabs.clear(); openEditorDroppedFile(tabs, local, 0);
    check(openEditorDroppedFile(tabs, ResourceId(L"d:\\外部 CSV\\FILE.csv"), 1) == 0 && tabs.size() == 1,
          "Dropping a Windows path with different case duplicated the tab");
    ProjectSession mixed; saveEditorTabs(mixed, tabs, 0);
    check(restoreEditorTabPath(mixed.tabs[0].path, wsl) == local,
          "Saving and restoring a Windows file in a WSL project changed its filesystem");
#endif
}
}
int main() {
    try {
        editorTabPathTests();
        editorTabTests();
        droppedFileTests();
        using poppy::projectDirectoryName;
        check(projectDirectoryName("D:\\repository\\yumayo\\PoppyWorktrees\\PoppyDirect2D") ==
              "-d-repository-yumayo-poppyworktrees-poppydirect2d", "Windows project directory format changed");
        check(projectDirectoryName("d:/repository/yumayo/PoppyWorktrees/PoppyDirect2D/") ==
              "-d-repository-yumayo-poppyworktrees-poppydirect2d", "Alternate separators changed the directory format");
        check(projectDirectoryName("\\\\server\\share\\project") == "-server-share-project", "UNC project format is incorrect");
        check(projectDirectoryName("C:\\") == "-c", "Drive-root project format is incorrect");
        const auto longName = projectDirectoryName("d:\\" + std::string(1000, 'a'));
        check(longName.size() <= 137, "Long paths exceed the directory component limit");
        check(longName != projectDirectoryName("d:\\" + std::string(999, 'a') + 'b'), "Truncating long paths lost their identity");
        check(projectDirectoryName("d:/a.b") == projectDirectoryName("d:/a-b"), "The collision fixture no longer collides");
        check(poppy::projectPathHash("d:/a.b") != poppy::projectPathHash("d:/a-b"), "Colliding readable names need different suffixes");
        const auto bash = poppy::terminalShellCommand(poppy::TerminalShell::GitBash, L"C:\\Windows\\System32");
        check(poppy::processCommandLine(bash) == L"\"C:\\Program Files\\Git\\bin\\bash.exe\" --login -i",
              "Git Bash needs a quoted executable path and an interactive login shell");
        const auto powershell = poppy::terminalShellCommand(poppy::TerminalShell::PowerShell, L"D:\\Windows\\System32");
        check(powershell.front() == L"D:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" &&
              powershell.back().find(L"UTF8Encoding") != std::wstring::npos,
              "PowerShell lost the system directory or UTF-8 initialization");
        std::cout << "Session and editor tab tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
