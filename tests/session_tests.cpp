#include "session.h"
#include "command_line.h"
#include "editor_tabs.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
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
    for (const auto kind : {GitDiffKind::Changes, GitDiffKind::Staged, GitDiffKind::Commit}) {
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
}
int main() {
    try {
        editorTabTests();
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
