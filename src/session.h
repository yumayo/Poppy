#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace poppy {
struct ViewState {
    int top = 0, left = 0;
    size_t anchor = 0, caret = 0;
    bool after = false;
};
struct SavedTab {
    std::string path;
    bool diff = false;
    ViewState fileView, diffView;
    bool pinned = false;
};
struct ProjectSession {
    int sidebar = 245, terminalHeight = 260;
    bool terminalVisible = true, gitSidebar = false, diffInline = false, gitTree = false;
    size_t activeTab = 0;
    std::vector<SavedTab> tabs;
    std::vector<std::string> expanded;
    std::string selected;
};
struct AppSession {
    std::string lastProject;
    int x = 0, y = 0, width = 1180, height = 820;
    bool positioned = false, maximized = false;
};

// Input is an absolute, normalized, case-folded Windows path in UTF-8.
std::string projectDirectoryName(std::string_view pathKey);
std::string projectPathHash(std::string_view pathKey);
}
