#pragma once
#include "content_search.h"
#include "edit_buffer.h"
#include "resource.h"
#include "session.h"
#include <memory>
#include <optional>

namespace poppy {
struct EditorNavigation;
class DockLayout;
struct EditorTab {
    ResourceId path; bool diff = false; ViewState fileView, diffView; bool pinned = false;
    std::shared_ptr<EditBuffer> buffer; int group = 0; bool displayed = false;
    GitDiffKind diffKind = GitDiffKind::Automatic; std::string revision; bool allChanges = false;
    std::optional<ContentMatch> searchTarget; std::string searchQuery; bool searchMatchCase = false;
    ResourceId gitRoot = {};
    std::shared_ptr<const Document> diffBefore = {};
    // Restored tabs stay permanent; only newly opened tabs start as previews.
    bool preview = false;
    std::shared_ptr<EditorNavigation> navigation = {};
    // Terminal sessions share the same tab strip and docking groups.
    std::string terminalId;
    // A file comparison shows this file on the left and path on the right.
    ResourceId comparePath = {};
};

// History retains metadata only, including previews replaced by another file.
struct EditorNavigation {
    EditorTab tab;
    bool closed = false;
};
void rememberEditorTab(EditorTab& tab);
bool sameEditorTab(const EditorTab& left, const EditorTab& right);
// Aggregate tabs have no single file path. Relative paths use the project root.
std::optional<std::wstring> editorTabPathText(const EditorTab& tab, const ResourceId& root, bool relative);

// Reuse an existing tab, or replace the clean preview in the target group.
size_t openEditorPreview(std::vector<EditorTab>& tabs, EditorTab target);
// New file tabs use the top row, creating a pane above terminals when necessary.
// Existing tabs keep their placement. Returns tabs.size() if the group limit is reached.
size_t openEditorFilePreview(std::vector<EditorTab>& tabs, EditorTab target, DockLayout& dock, int preferred);
void keepModifiedEditorTabs(std::vector<EditorTab>& tabs);
// Preview tabs are session-local. Repair selection and groups after filtering.
void saveEditorTabs(ProjectSession& state, const std::vector<EditorTab>& tabs, size_t active);
}
