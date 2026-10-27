#include "tree_model.h"
#include "git_changes.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace poppy;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void hierarchy() {
    TreeModel tree;
    const auto root = tree.insert(0, L"workspace 日本語", true, 10, 1);
    check(root && !tree.insert(99999, L"invalid", false, 0, 0), "Invalid parent accepted");
    check(tree.expand(root, true) && tree.visible().size() == 1, "An empty folder must remain expanded while loading");
    const auto dir = tree.insert(root, L"folder", true, 20, 2);
    const auto file = tree.insert(root, L"file.txt", false, 30, 3);
    const auto nested = tree.insert(dir, L"nested.txt", false, 40, 4);
    check(tree.child() == root && tree.child(root) == dir && tree.next(dir) == file && !tree.next(file), "Sibling ordering is incorrect");
    check(tree.visible().size() == 3 && !tree.expand(file, true), "Collapsed descendants or leaf expansion are incorrect");
    tree.expand(dir, true);
    const auto rows = tree.visible();
    check(rows.size() == 4 && rows[2].item == nested && rows[2].depth == 2 && rows[3].item == file, "Visible rows are not in preorder");
    tree.select(nested);
    check(!tree.select(99999) && tree.selection() == nested, "Invalid selection replaced the current item");
    tree.expand(dir, false);
    check(tree.selection() == dir && tree.visible().size() == 3, "Collapsing a selected descendant did not select its ancestor");
    tree.label(file, L"renamed 日本語.txt", 55);
    check(tree.get(file)->text == L"renamed 日本語.txt" && tree.get(file)->color == 55, "Label or Git color update was lost");
    tree.expand(dir, true);
    check(tree.visible().size() == 4, "Re-expansion lost the retained hierarchy");
    tree.select(nested);
    const auto removed = tree.eraseChildren(dir);
    check(removed.size() == 1 && removed[0].second == 40 && tree.selection() == dir && !tree.child(dir), "Removing descendants did not release data or restore selection");
    check(tree.get(dir)->expanded && tree.get(dir)->expandable, "An empty expanded directory lost its expansion intent");
    const auto replacement = tree.insert(dir, L"new.txt", false, 50, 5);
    check(replacement != nested && !tree.get(nested) && tree.visible().size() == 4, "A stale ID was reused or a new child stayed hidden");
    const auto cleared = tree.clear();
    check(cleared.size() == 4 && tree.visible().empty() && !tree.selection() && !tree.child(), "Clearing a workspace retained tree state");
    check(tree.insert(0, L"other", true, 0, 0) > replacement, "Clearing the tree reused asynchronous item IDs");
}
void orderingAndRemoval() {
    TreeModel tree;
    const auto root = tree.insert(0, L"root", true, 0, 0);
    const auto third = tree.insert(root, L"third", false, 3, 0);
    const auto first = tree.insert(root, L"first", false, 1, 0);
    const auto second = tree.insert(root, L"second", true, 2, 0);
    const auto nested = tree.insert(second, L"nested", false, 4, 0);
    tree.expand(root, true); tree.expand(second, true); tree.select(nested);
    tree.sort(root, [](uintptr_t a, uintptr_t b) { return a < b; });
    check(tree.child(root) == first && tree.next(first) == second && tree.next(second) == third && !tree.next(third), "Sorting did not update sibling traversal");
    check(tree.visible()[2].item == second && tree.selection() == nested, "Sorting changed selection or expansion state");
    check(tree.erase(second).size() == 2 && tree.selection() == root, "Subtree removal lost its data or selected a deleted item");
    check(tree.next(first) == third && !tree.get(nested), "Subtree removal left broken siblings or descendants");
    tree.erase(first);
    check(tree.child(root) == third && !tree.next(third), "Removing the first sibling broke traversal");
    tree.erase(third);
    check(!tree.child(root) && tree.visible().size() == 1, "Removing the last sibling left a visible row");
    check(tree.erase(99999).empty(), "Removing an unknown item changed the model");
}
void largeAndDeepTrees() {
    TreeModel tree;
    const auto root = tree.insert(0, L"root", true, 0, 0); tree.expand(root, true);
    for (uintptr_t i = 1; i <= 50000; ++i) tree.insert(root, L"file.txt", false, i, 0);
    check(tree.visible().size() == 50001, "A large folder lost visible rows");
    size_t count = 0;
    for (auto item = tree.child(root); item; item = tree.next(item)) ++count;
    check(count == 50000, "Large-folder sibling traversal failed");
    const auto released = tree.eraseChildren(root);
    check(released.size() == 50000 && tree.size() == 1 && tree.visible().size() == 1, "Collapsing a large folder did not release all descendants");
    tree.clear();
    TreeItem parent = 0, top = 0;
    for (uintptr_t i = 0; i < 10000; ++i) {
        parent = tree.insert(parent, L"nested", true, i, 0); tree.expand(parent, true);
        if (!top) top = parent;
    }
    check(tree.visible().size() == 10000 && tree.visible().back().depth == 9999, "Deep-tree traversal failed");
    tree.select(parent); tree.expand(top, false);
    check(tree.selection() == top && tree.visible().size() == 1, "Deep-tree collapse failed");
    check(tree.erase(top).size() == 10000 && tree.size() == 0, "Deep-tree removal failed");
}
void movingSubtrees() {
    TreeModel tree;
    const auto root = tree.insert(0, L"root", true, 0, 0);
    const auto source = tree.insert(root, L"source", true, 1, 0);
    const auto target = tree.insert(root, L"target", true, 2, 0);
    const auto first = tree.insert(source, L"first", false, 3, 0);
    const auto directory = tree.insert(source, L"日本語 folder", true, 4, 0);
    const auto last = tree.insert(source, L"last", false, 5, 0);
    const auto nested = tree.insert(directory, L"nested.txt", false, 6, 0);
    const auto existing = tree.insert(target, L"existing", false, 7, 0);
    tree.expand(root, true); tree.expand(source, true); tree.expand(directory, true); tree.expand(target, true);
    tree.select(nested);
    check(!tree.reparent(root, target) && !tree.reparent(source, directory) && !tree.reparent(source, source) &&
        !tree.reparent(directory, source) && !tree.reparent(directory, first) && !tree.reparent(99999, target) && !tree.reparent(directory, 99999),
        "An invalid move changed the hierarchy");
    check(tree.reparent(directory, target), "Could not move a folder to another parent");
    check(tree.parent(directory) == target && tree.parent(nested) == directory && tree.selection() == nested &&
        tree.get(directory)->expanded && tree.get(nested)->data == 6 && tree.size() == 8,
        "Moving a folder lost its descendants, expansion, selection or data");
    check(tree.child(source) == first && tree.next(first) == last && !tree.next(last) &&
        tree.next(existing) == directory && !tree.next(directory), "Moving a middle sibling broke traversal");
    check(tree.visible().size() == 8 && tree.visible().back().item == nested && tree.visible().back().depth == 3,
        "Moving did not rebuild visible rows and depths");
    check(tree.reparent(first, target) && tree.child(source) == last && tree.reparent(last, target) && !tree.child(source),
        "Moving first and last siblings left stale links");
    check(tree.reparent(directory, root) && tree.visible().back().item == nested && tree.visible().back().depth == 2,
        "Moving to the root folder lost the subtree");
    check(tree.erase(target).size() == 4 && tree.get(nested) && tree.selection() == nested,
        "Removing the old parent removed a moved subtree");
}
void gitChangeLayouts() {
    const std::vector<std::wstring> paths{L"z.txt", L"src/z.cpp", L"src/nested/same.cpp", L"src/A.cpp",
        L"docs/same.cpp", L"docs/日本語 file.txt", L"a.txt"};
    const auto list = buildGitChanges(paths, false);
    check(list.size() == paths.size(), "List layout added folders or dropped files");
    for (size_t i = 0; i < list.size(); ++i)
        check(!list[i].directory() && list[i].path == paths[i] && list[i].entry == i && list[i].parent == noGitChange,
            "List layout lost a relative path, order or file mapping");
    const auto nodes = buildGitChanges(paths, true);
    // Only compare the ASCII names' order: Unicode collation depends on locale.
    check(nodes.size() == paths.size() + 3 && nodes[0].path == L"docs" && nodes[3].path == L"src" &&
        nodes[4].path == L"src/nested" && nodes[6].path == L"src/A.cpp" && nodes[7].path == L"src/z.cpp" &&
        nodes[8].path == L"a.txt" && nodes[9].path == L"z.txt", "Tree folders and files are not grouped and sorted");
    TreeModel tree;
    std::vector<TreeItem> items;
    size_t files = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        check(node.parent == noGitChange || node.parent < i, "A child precedes its parent");
        if (!node.directory()) { ++files; check(node.entry < paths.size() && paths[node.entry] == node.path, "Tree layout opened a different file"); }
        if (node.parent != noGitChange)
            check(nodes[node.parent].directory() && node.path.rfind(nodes[node.parent].path + L"/", 0) == 0, "A file has the wrong parent");
        items.push_back(tree.insert(node.parent == noGitChange ? 0 : items[node.parent], node.path, node.directory(), node.entry, 0));
        if (node.directory()) tree.expand(items.back(), true);
    }
    check(files == paths.size() && tree.visible().size() == nodes.size(), "Tree layout lost changed files");
    tree.select(items[5]); tree.expand(items[3], false);
    check(tree.selection() == items[3] && tree.visible().size() == nodes.size() - 4, "A Git folder did not collapse its descendants");
    tree.expand(items[3], true);
    check(tree.visible().size() == nodes.size(), "Reopening a Git folder lost changes");
    check(buildGitChanges({}, true).empty() && buildGitChanges({}, false).empty(), "An empty change list has rows");
    const auto limited = buildGitChanges({L"a/b/c.txt", L"a/b/d.txt", L"other/file.txt"}, true, 1);
    check(limited.size() == 4 && limited[0].directory(), "The directory limit was ignored");
    files = 0;
    for (const auto& node : limited) if (!node.directory()) ++files;
    check(files == 3, "The directory limit hid changed files");
    std::vector<std::wstring> many;
    for (int i = 0; i < 50000; ++i) many.push_back(L"src/file-" + std::to_wstring(i) + L".cpp");
    check(buildGitChanges(many, true).size() == 50001 && buildGitChanges(many, false).size() == 50000,
        "A large Git change list lost files");
}
}
int main() {
    try { hierarchy(); orderingAndRemoval(); largeAndDeepTrees(); movingSubtrees(); gitChangeLayouts(); std::cout << "Tree model tests passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
