#pragma once
#include "git_history.h"
#include "content_search.h"
#include "terminal_profile.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
struct ViewState {
    int top = 0, left = 0;
    size_t anchor = 0, caret = 0;
    bool after = false;
    // Byte offset of the top continuation within the logical line in top.
    size_t wrapOffset = 0;
    // A large file's view also names its rows by file byte, since another
    // window of the file may be loaded when the state is restored: the top
    // row, the caret's record, and the anchor / caret (a table's cells, with
    // their columns). end keeps a view that followed the file's end there.
    struct FileCells {
        uint64_t top = 0, record = 0, anchor = 0, caret = 0;
        size_t anchorColumn = 0, caretColumn = 0;
        bool end = false;
    };
    std::optional<FileCells> file = std::nullopt;
};
struct SavedTab {
    std::string path;
    bool diff = false;
    ViewState fileView, diffView;
    bool pinned = false;
    int group = 0;
    bool displayed = false;
    GitDiffKind diffKind = GitDiffKind::Automatic;
    std::string revision = {};
    bool allChanges = false;
    std::string gitRoot = {};
    std::string comparePath = {};
};
struct SavedTerminal {
    std::string id = "shell-1", name = "Shell", directory;
    int group = 0;
    bool displayed = false;
    TerminalShell shell = TerminalShell::PowerShell;
    std::string hostSession = {};
    bool pinned = false;
};
struct SavedSearch {
    std::string query, status = "Type to search this project.";
    bool matchCase = false, respectIgnore = true;
    size_t selected = 0;
    int top = 0;
    std::vector<ContentMatch> matches;
};
struct ProjectSession {
    std::string wslUid;
    std::string dockLayout;
    int sidebar = 245, terminalHeight = 260;
    bool terminalVisible = true, gitSidebar = false, diffInline = true, gitTree = true;
    bool branchSidebar = false;
    size_t activeTab = 0;
    std::vector<SavedTab> tabs;
    std::vector<std::string> expanded;
    std::string selected;
    int explorerTop = 0, explorerLeft = 0, gitTop = 0, gitLeft = 0;
    size_t firstTabRow = 0;
    size_t rightTabRow = 0;
    int editorRatio = 500, terminalRatio = 500;
    std::string gitSelected, focusedPane = "viewer", activeTerminal = "shell-1";
    std::vector<std::string> collapsedGit;
    std::vector<SavedTerminal> terminals = {SavedTerminal{}};
    TerminalShell defaultShell = TerminalShell::PowerShell;
    bool searchSidebar = false;
    // CONTEXT: Explorer showing only the project's .context folder.
    bool contextSidebar = false;
    SavedSearch search;
};
struct AppSession {
    std::string lastProject;
    std::vector<std::string> openProjects, openProjectKeys;
    int projectListWidth = 180;
    bool projectListCollapsed = false;
    int x = 0, y = 0, width = 1180, height = 820;
    bool positioned = false, maximized = false;
};

// Input is an absolute, normalized, case-folded Windows path in UTF-8.
std::string projectDirectoryName(std::string_view pathKey);
std::string projectPathHash(std::string_view pathKey);
}
