#pragma once
#include "scrollbars.h"
#include "tree_messages.h"
#include "tree_input.h"
#include "message_popup.h"
#include <optional>

namespace poppy {
class Tree {
public:
    // Batch hierarchy/label changes without changing WS_VISIBLE on hidden panes.
    class Batch {
    public:
        explicit Batch(Tree& tree) : tree_(tree) { ++tree_.batch_; }
        ~Batch() { if (!--tree_.batch_) tree_.layout(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
    private:
        Tree& tree_;
    };
    void create(HWND parent, HINSTANCE instance, const wchar_t* title, int id);
    void setDpi(UINT dpi, int rowHeight = 26);
    HWND window() const { return window_; }
    TreeItem insert(TreeItem parent, std::wstring text, bool expandable, uintptr_t data, COLORREF color = foreground);
    const TreeModel::Node* item(TreeItem id) const { return model_.get(id); }
    void label(TreeItem id, std::wstring text, COLORREF color);
    void erase(TreeItem id);
    void clearChildren(TreeItem id);
    void clear();
    void sort(TreeItem parent, const std::function<bool(uintptr_t, uintptr_t)>& compare);
    void reparent(TreeItem id, TreeItem parent) { if (model_.reparent(id, parent)) changed(); }
    void expand(TreeItem id, bool expanded);
    void collapseAll();
    void select(TreeItem id, bool activate = true);
    void addSelection(TreeItem id) { if (!model_.selected(id) && model_.select(id, true, false)) changed(); }
    void showSelection(bool centered = false) { ensureVisible(centered); changed(); }
    std::function<void()> selectionChanged;
    TreeItem child(TreeItem parent = 0) const { return model_.child(parent); }
    TreeItem next(TreeItem id) const { return model_.next(id); }
    int top() const { return top_; }
    int left() const { return MulDiv(left_, 96, dpi_); }
    void restoreScroll(int top, int left) { top_ = top; left_ = MulDiv(left, dpi_, 96); changed(); }
    TreeItem selection() const { return model_.selection(); }
    std::vector<TreeItem> selections(bool rootsOnly = false) const { return model_.selections(rootsOnly); }
    bool multipleSelection = false;
    bool dragging() const { return drag_.has_value(); }
    void rename(TreeItem item, const std::wstring& name);
    void createItem(TreeItem parent, bool directory);
    std::function<bool(TreeItem)> canCreate;
    std::function<void(TreeItem, const std::wstring&, bool)> createCommitted;
    std::function<bool(TreeItem)> canRename;
    std::function<void(TreeItem)> renameRequested;
    std::function<void(TreeItem, const std::wstring&)> renameCommitted;
    std::function<bool(TreeItem)> canDelete;
    std::function<void(const std::vector<TreeItem>&)> deleteRequested;
    std::function<bool(TreeItem)> canCopy;
    std::function<void(const std::vector<TreeItem>&, bool)> copyRequested;
    std::function<bool(TreeItem)> canPasteFiles;
    std::function<void(TreeItem)> pasteFilesRequested;
    std::function<bool(TreeItem)> canDrag;
    std::function<bool(TreeItem, TreeItem)> canDrop;
    std::function<void(const std::vector<TreeItem>&, TreeItem)> moveCommitted;
    std::function<bool(TreeItem)> canOpenWithDefaultApp;
    std::function<void(TreeItem)> openWithDefaultAppRequested;
    std::function<bool(TreeItem)> canRevealInFileExplorer;
    std::function<void(TreeItem)> revealInFileExplorerRequested;
    std::function<bool(TreeItem)> canPasteImage;
    std::function<void(TreeItem)> pasteImageRequested;
    std::function<bool()> canUndo, canRedo;
    std::function<void()> undoRequested, redoRequested;
    std::function<void(TreeItem)> activated;
    std::function<void(TreeItem)> doubleClicked;
    std::function<void(TreeItem, bool)> expanded;
    std::function<void(uintptr_t)> removed;
    std::function<std::wstring(TreeItem)> actionLabel;
    std::function<Renderer::Icon(TreeItem)> actionIcon;
    std::function<void(TreeItem)> actionClicked;
    std::function<std::wstring(TreeItem)> secondaryActionLabel;
    std::function<Renderer::Icon(TreeItem)> secondaryActionIcon;
    std::function<void(TreeItem)> secondaryActionClicked;
    std::function<std::wstring(TreeItem)> tertiaryActionLabel;
    std::function<Renderer::Icon(TreeItem)> tertiaryActionIcon;
    std::function<void(TreeItem)> tertiaryActionClicked;
    std::function<std::wstring(TreeItem)> quaternaryActionLabel;
    std::function<Renderer::Icon(TreeItem)> quaternaryActionIcon;
    std::function<void(TreeItem)> quaternaryActionClicked;
    bool hoverActionsOnly = false;
    std::function<std::wstring(TreeItem)> hoverMessage;
    std::function<bool(TreeItem)> dimmed;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    void beginInput(TreeItem item, const std::wstring& name, bool creating, bool directory = false);
    void finishRename(bool commit);
    void updateDrag(POINT point);
    void tickDrag();
    void endDrag(bool drop = false);
    bool selectionAllowed(const std::function<bool(TreeItem)>& allowed) const;
    void layoutRename();
    void imePosition();
    void typeSearch(const std::wstring& text, bool composed = false);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void layout();
    void changed();
    void ensureVisible(bool centered = false);
    const std::vector<TreeModel::Row>& rows() const;
    int row(TreeItem id) const;
    TreeItem hit(int y) const;
    RECT actionBounds(TreeItem id, unsigned slot = 0) const;
    bool actionVisible(TreeItem id, unsigned slot = 0) const;
    COLORREF textColor(TreeItem id) const;
    const std::function<std::wstring(TreeItem)>& actionLabelFor(unsigned slot) const;
    void hover(POINT point);
    void cancelMessage();
    void showMessage();
    void scroll(int vertical, int horizontal = 0);
    void release(const std::vector<std::pair<TreeItem, uintptr_t>>& items);
    int page() const;
    HWND window_ = nullptr;
    HWND rename_ = nullptr;
    TreeInput renameInput_;
    HWND actionTips_ = nullptr;
    TreeItem hotItem_ = 0;
    int hotAction_ = -1;
    bool trackingMouse_ = false;
    std::wstring actionTip_;
    MessagePopup messagePopup_;
    TreeItem messageItem_ = 0;
    TreeItem renaming_ = 0;
    bool creating_ = false, creatingDirectory_ = false;
    Renderer renderer_;
    ImeComposition ime_;
    InputLine searchInput_;
    RECT searchBounds_{};
    Scrollbars scrollbars_;
    TreeModel model_;
    // The new-item input occupies a visual row without becoming a resource node.
    mutable std::vector<TreeModel::Row> creationRows_;
    mutable bool rowsDirty_ = true;
    std::unordered_map<TreeItem, int> widths_;
    UINT dpi_ = 96;
    int rowHeight_ = 26, indent_ = 18, top_ = 0, left_ = 0, wheel_ = 0, batch_ = 0;
    bool layingOut_ = false;
    std::wstring search_;
    ULONGLONG searchTime_ = 0;
    struct Drag {
        TreeItem source, target = 0;
        POINT origin{}, point{};
        bool active = false;
        ULONGLONG hoverTime = 0;
    };
    std::optional<Drag> drag_;
};
}
