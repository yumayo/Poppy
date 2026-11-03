#include "navigation_history.h"
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void timelineTests() {
    NavigationHistory history;
    NavigationTarget active;
    const NavigationTarget a{1, std::make_shared<EditorNavigation>(), {}},
        b{1, {}, "shell-1"}, c{1, {}, "shell-2"}, d{2, {}, "shell-1"};
    const auto activate = [&](const NavigationTarget& target) {
        active = target;
        // Project activation can focus an intermediate pane and the target.
        history.visit(d); history.visit(target);
        check(!history.move(false, [](const auto&) { return true; }), "Nested navigation was accepted");
        return true;
    };
    check(!history.move(false, activate) && !history.move(true, activate), "Empty history moved");
    history.visit({}); history.visit(a);
    check(!history.move(false, activate), "A single entry moved");
    history.visit(a); history.visit(b); history.visit(c); history.visit(d);
    check(history.move(false, activate) && active == c, "Back lost a terminal or project identity");
    check(history.move(false, activate) && active == b, "Back skipped a terminal");
    check(history.move(false, activate) && active == a, "Back did not return to the editor");
    check(!history.move(false, activate), "Consecutive duplicate activations were retained");
    check(history.move(true, activate) && active == b, "Forward lost terminal focus");
    history.visit(b); // Reselecting the current target must preserve Forward.
    check(history.move(true, activate) && active == c, "Reselecting a target cleared Forward");
    check(history.move(false, activate) && active == b, "Back failed before branching");
    history.visit(d);
    check(!history.move(true, activate), "A new activation retained the old forward branch");
    check(history.move(false, activate) && active == b, "Branching damaged earlier history");
}
void unavailableTests() {
    NavigationHistory history;
    const NavigationTarget a{1, std::make_shared<EditorNavigation>(), {}},
        b{1, std::make_shared<EditorNavigation>(), {}}, c{2, {}, "closed-project"}, d{1, {}, "current"};
    history.visit(a); history.visit(b); history.visit(c); history.visit(d);
    b.editor->closed = true;
    NavigationTarget active;
    const auto activate = [&](const NavigationTarget& target) {
        if (target.project == 2 || (target.editor && target.editor->closed)) return false;
        active = target; return true;
    };
    check(history.move(false, activate) && active == a, "Back did not skip closed tabs/projects");
    check(!history.move(true, [](const auto&) { return false; }), "Unavailable targets were accepted");
    check(history.move(true, activate) && active == d, "Failed navigation changed the cursor");
    history.visit(a); // d, a; repeated a behind dead entries must not consume a click.
    check(history.move(false, activate) && active == d, "Repeated visits lost their order");
    NavigationHistory duplicates;
    duplicates.visit(a); duplicates.visit(b); duplicates.visit(a);
    check(!duplicates.move(false, activate), "Dead entries caused navigation to the same target");
    try { history.move(false, [](const auto&) -> bool { throw std::runtime_error("activation failed"); }); }
    catch (const std::runtime_error&) {}
    check(history.move(false, activate) && active == a, "An exception left navigation disabled");
}
void previewTests() {
    std::vector<EditorTab> tabs;
    EditorTab first; first.path = ResourceId(std::filesystem::temp_directory_path() / "a.txt");
    first.buffer = std::make_shared<EditBuffer>();
    openEditorPreview(tabs, first); rememberEditorTab(tabs[0]);
    const auto previous = tabs[0].navigation;
    std::weak_ptr<EditBuffer> body = tabs[0].buffer;
    first.buffer.reset();
    tabs[0].fileView.top = 37; rememberEditorTab(tabs[0]);
    check(!previous->tab.navigation && !previous->tab.buffer && !previous->tab.diffBefore, "History retained document data or a reference cycle");
    EditorTab second; second.path = ResourceId(std::filesystem::temp_directory_path() / "b.txt");
    openEditorPreview(tabs, second);
    check(body.expired(), "A replaced preview's document was retained by history");
    auto restored = previous->tab; restored.navigation = previous;
    const auto index = openEditorPreview(tabs, restored);
    check(tabs.size() == 1 && tabs[index].path == first.path && tabs[index].fileView.top == 37 && tabs[index].navigation == previous,
        "Preview navigation lost identity, position or replacement behavior");
    EditorTab staged; staged.path = first.path; staged.diff = true; staged.diffKind = GitDiffKind::Staged;
    staged.gitRoot = first.path.parent_path(); staged.group = 1;
    rememberEditorTab(staged);
    check(staged.navigation->tab.diffKind == GitDiffKind::Staged && staged.navigation->tab.gitRoot == staged.gitRoot && staged.navigation->tab.group == 1,
        "History lost the Git comparison or split group");
    EditorTab wsl; wsl.path = ResourceId::wsl("Ubuntu", "/work/日本語.txt", "developer");
    wsl.diffKind = GitDiffKind::Commit; wsl.revision = "0123456789abcdef";
    rememberEditorTab(wsl);
    check(wsl.navigation->tab.path == wsl.path && wsl.navigation->tab.revision == wsl.revision,
        "History lost WSL authority or commit identity");
    EditorTab reopened = first; rememberEditorTab(reopened);
    NavigationHistory history;
    history.visit({1, previous, {}}); history.visit({1, {}, "closed-terminal"}); history.visit({1, reopened.navigation, {}});
    check(!history.move(false, [](const auto&) { return false; }), "Navigation accepted a closed terminal");
    size_t attempts = 0;
    check(!history.move(false, [&](const auto&) { ++attempts; return false; }) && attempts == 1,
        "Aliases of the same preview consumed a navigation step");
    check(!(NavigationTarget{1, previous, {}} == NavigationTarget{2, previous, {}}), "Projects shared an editor history identity");
    check(!(NavigationTarget{1, previous, {}} == NavigationTarget{1, staged.navigation, {}}), "File and staged diff shared a history identity");
}
void retentionTests() {
    NavigationHistory history;
    auto editor = std::make_shared<EditorNavigation>();
    std::weak_ptr<EditorNavigation> old = editor;
    history.visit({1, editor, {}}); editor.reset();
    for (size_t i = 0; i < NavigationHistory::limit; ++i) history.visit({1, {}, std::to_string(i)});
    check(old.expired(), "History exceeded its retention limit");
    size_t count = 0;
    while (history.move(false, [&](const auto&) { ++count; return true; })) {}
    check(count == NavigationHistory::limit - 1, "History retained the wrong number of entries");
    NavigationHistory branch;
    branch.visit({1, {}, "first"});
    editor = std::make_shared<EditorNavigation>(); old = editor;
    branch.visit({1, editor, {}}); editor.reset();
    check(branch.move(false, [](const auto&) { return true; }), "Cannot return before a preview");
    branch.visit({1, {}, "new"});
    check(old.expired(), "Discarding Forward retained preview metadata");
}
}
int main() {
    try {
        timelineTests(); unavailableTests(); previewTests(); retentionTests();
        std::cout << "Navigation history tests passed.\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
