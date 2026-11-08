#include "editor_tabs.h"
#include "dock_layout.h"
#include <algorithm>
#include <set>

namespace poppy {
std::optional<std::wstring> editorTabPathText(const EditorTab& tab, const ResourceId& root, bool relative) {
    if (!tab.terminalId.empty() || tab.allChanges || tab.path.empty() || (relative && root.empty())) return std::nullopt;
    auto path = relative ? tab.path.lexically_relative(root) : tab.path;
    if (path.empty()) return std::nullopt;
    return path.make_preferred().native();
}
ResourceId restoreEditorTabPath(std::string_view saved, const ResourceId& root) {
    if (root.wsl() && !saved.empty() && saved.front() == '/' && saved.substr(0, 2) != "//")
        return root.at(std::string(saved));
    const auto path = ResourceId::parse(saved);
    return path.wsl() || path.is_absolute() ? path : root.at(std::string(saved));
}
size_t openEditorDroppedFile(std::vector<EditorTab>& tabs, const ResourceId& path, int group) {
    keepModifiedEditorTabs(tabs);
    EditorTab target; target.path = path; target.group = group;
    const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const EditorTab& tab) { return sameEditorTab(tab, target); });
    if (found != tabs.end()) {
        found->preview = false; found->diff = false;
        return static_cast<size_t>(found - tabs.begin());
    }
    for (const auto& tab : tabs) if (tab.path.key() == path.key() && tab.buffer) { target.buffer = tab.buffer; break; }
    tabs.push_back(std::move(target));
    return tabs.size() - 1;
}
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
        left.allChanges == right.allChanges && left.gitRoot.key() == right.gitRoot.key() && left.comparePath.key() == right.comparePath.key();
}
size_t openEditorFilePreview(std::vector<EditorTab>& tabs, EditorTab target, DockLayout& dock, int preferred) {
    const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const EditorTab& tab) { return sameEditorTab(tab, target); });
    if (found != tabs.end()) return openEditorPreview(tabs, std::move(target));
    std::vector<int> terminals;
    for (const auto& tab : tabs) if (tab.displayed && !tab.terminalId.empty()) terminals.push_back(tab.group);
    target.group = dock.topGroup(preferred, terminals);
    if (target.group < 0) target.group = dock.nodes().size() == 1 ? dock.topGroup() : dock.split(dock.topGroup(), DockSide::Top);
    if (target.group < 0) return tabs.size();
    return openEditorPreview(tabs, std::move(target));
}
size_t openEditorPreview(std::vector<EditorTab>& tabs, EditorTab target) {
    keepModifiedEditorTabs(tabs);
    const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const EditorTab& tab) {
        return sameEditorTab(tab, target);
    });
    if (found != tabs.end()) return static_cast<size_t>(found - tabs.begin());
    target.preview = true;
    // Preserve the shared working buffer when switching between file and diff.
    if (target.comparePath.empty() && !target.allChanges && (target.diffKind == GitDiffKind::Automatic || target.diffKind == GitDiffKind::Changes)) {
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
        if (!tab.terminalId.empty()) continue;
        if (tab.preview && !tab.pinned && (!tab.buffer || (!tab.buffer->dirty() && !tab.buffer->saving))) continue;
        state.tabs.push_back({tab.path.u8string(), tab.diff, tab.fileView, tab.diffView, tab.pinned, tab.group,
            tab.displayed, tab.diffKind, tab.revision, tab.allChanges, tab.gitRoot.u8string(), tab.comparePath.u8string()});
    }
    if (state.dockLayout.empty() && std::none_of(state.tabs.begin(), state.tabs.end(), [](const SavedTab& tab) { return tab.group == 0; })) {
        for (auto& tab : state.tabs) tab.group = 0;
        activeGroup = 0;
    }
    std::set<int> groups;
    for (const auto& tab : state.tabs) groups.insert(tab.group);
    for (int group : groups) {
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
