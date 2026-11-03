#include "editor_tabs.h"
#include <algorithm>

namespace poppy {
void rememberEditorTab(EditorTab& tab) {
    if (!tab.navigation) tab.navigation = std::make_shared<EditorNavigation>();
    auto& saved = tab.navigation->tab;
    saved = tab;
    saved.navigation.reset();
    saved.buffer.reset(); saved.diffBefore.reset(); saved.searchTarget.reset();
}
void keepModifiedEditorTabs(std::vector<EditorTab>& tabs) {
    for (auto& tab : tabs)
        if (tab.pinned || (tab.buffer && (tab.buffer->dirty() || tab.buffer->saving))) tab.preview = false;
}
bool sameEditorTab(const EditorTab& left, const EditorTab& right) {
    return left.path.key() == right.path.key() && left.diffKind == right.diffKind && left.revision == right.revision &&
        (left.diffKind != GitDiffKind::Commit || left.diff == right.diff) &&
        left.allChanges == right.allChanges && left.gitRoot.key() == right.gitRoot.key();
}
size_t openEditorPreview(std::vector<EditorTab>& tabs, EditorTab target) {
    keepModifiedEditorTabs(tabs);
    const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const EditorTab& tab) {
        return sameEditorTab(tab, target);
    });
    if (found != tabs.end()) return static_cast<size_t>(found - tabs.begin());
    target.preview = true;
    // Preserve the shared working buffer when switching between file and diff.
    if (!target.allChanges && (target.diffKind == GitDiffKind::Automatic || target.diffKind == GitDiffKind::Changes)) {
        for (const auto& tab : tabs) if (tab.path.key() == target.path.key() && tab.buffer) { target.buffer = tab.buffer; break; }
    }
    if (target.pinned || (target.buffer && (target.buffer->dirty() || target.buffer->saving))) target.preview = false;
    for (size_t i = 0; i < tabs.size(); ++i) if (tabs[i].group == target.group && tabs[i].preview) {
        tabs[i] = std::move(target);
        return i;
    }
    tabs.push_back(std::move(target));
    return tabs.size() - 1;
}
void saveEditorTabs(ProjectSession& state, const std::vector<EditorTab>& tabs, size_t active) {
    state.tabs.clear(); state.activeTab = 0;
    int activeGroup = active < tabs.size() ? tabs[active].group : 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        const auto& tab = tabs[i];
        if (tab.preview && !tab.pinned && (!tab.buffer || (!tab.buffer->dirty() && !tab.buffer->saving))) continue;
        state.tabs.push_back({tab.path.u8string(), tab.diff, tab.fileView, tab.diffView, tab.pinned, tab.group,
            tab.displayed, tab.diffKind, tab.revision, tab.allChanges, tab.gitRoot.u8string()});
    }
    if (std::none_of(state.tabs.begin(), state.tabs.end(), [](const SavedTab& tab) { return tab.group == 0; })) {
        for (auto& tab : state.tabs) tab.group = 0;
        activeGroup = 0;
    }
    for (int group = 0; group < 2; ++group) {
        auto selected = std::find_if(state.tabs.begin(), state.tabs.end(), [group](const SavedTab& tab) { return tab.group == group && tab.displayed; });
        if (selected == state.tabs.end()) selected = std::find_if(state.tabs.begin(), state.tabs.end(), [group](const SavedTab& tab) { return tab.group == group; });
        if (selected != state.tabs.end()) {
            selected->displayed = true;
            if (group == activeGroup) state.activeTab = static_cast<size_t>(selected - state.tabs.begin());
        }
    }
    if (!state.tabs.empty() && !state.tabs[state.activeTab].displayed) {
        const auto selected = std::find_if(state.tabs.begin(), state.tabs.end(), [](const SavedTab& tab) { return tab.displayed; });
        state.activeTab = static_cast<size_t>(selected - state.tabs.begin());
    }
}
}
