#pragma once
#include "editor_tabs.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace poppy {
struct NavigationTarget {
    uint64_t project = 0;
    std::shared_ptr<EditorNavigation> editor;
    std::string terminal;
    bool operator==(const NavigationTarget& other) const {
        return project == other.project && terminal == other.terminal &&
            (editor == other.editor || (editor && other.editor && !editor->tab.path.empty() && sameEditorTab(editor->tab, other.editor->tab)));
    }
};

// One session-wide timeline shared by editor tabs and terminal sessions.
class NavigationHistory {
public:
    static constexpr size_t limit = 1000;
    void visit(NavigationTarget target);
    bool move(bool forward, const std::function<bool(const NavigationTarget&)>& activate);
private:
    std::vector<NavigationTarget> entries_;
    size_t current_ = 0;
    bool navigating_ = false;
};
}
