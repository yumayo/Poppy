#pragma once
#include "scrollbars.h"
#include "tree_messages.h"
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
    void select(TreeItem id, bool activate = true);
    TreeItem child(TreeItem parent = 0) const { return model_.child(parent); }
    TreeItem next(TreeItem id) const { return model_.next(id); }
    int top() const { return top_; }
    int left() const { return MulDiv(left_, 96, dpi_); }
    void restoreScroll(int top, int left) { top_ = top; left_ = MulDiv(left, dpi_, 96); changed(); }
    TreeItem selection() const { return model_.selection(); }
    bool dragging() const { return drag_.has_value(); }
    void rename(TreeItem item, const std::wstring& name);
    void createItem(TreeItem parent, bool directory);
    std::function<bool(TreeItem)> canCreate;
    std::function<void(TreeItem, const std::wstring&, bool)> createCommitted;
    std::function<bool(TreeItem)> canRename;
    std::function<void(TreeItem)> renameRequested;
    std::function<void(TreeItem, const std::wstring&)> renameCommitted;
    std::function<bool(TreeItem)> canDrag;
    std::function<bool(TreeItem, TreeItem)> canDrop;
    std::function<void(TreeItem, TreeItem)> moveCommitted;
    std::function<bool(TreeItem)> canOpenWithDefaultApp;
    std::function<void(TreeItem)> openWithDefaultAppRequested;
    std::function<bool(TreeItem)> canRevealInFileExplorer;
    std::function<void(TreeItem)> revealInFileExplorerRequested;
    std::function<bool(TreeItem)> canPasteImage;
    std::function<void(TreeItem)> pasteImageRequested;
    std::function<void(TreeItem)> activated;
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
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK renameProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void finishRename(bool commit);
    void updateDrag(POINT point);
    void tickDrag();
    void endDrag(bool drop = false);
    void layoutRename();
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void layout();
    void changed();
    void ensureVisible();
    int row(TreeItem id) const;
    TreeItem hit(int y) const;
    RECT actionBounds(TreeItem id, unsigned slot = 0) const;
    bool actionVisible(TreeItem id, unsigned slot = 0) const;
    const std::function<std::wstring(TreeItem)>& actionLabelFor(unsigned slot) const;
    void hover(POINT point);
    void scroll(int vertical, int horizontal = 0);
    void release(const std::vector<std::pair<TreeItem, uintptr_t>>& items);
    int page() const;
    HWND window_ = nullptr;
    HWND rename_ = nullptr;
    HWND actionTips_ = nullptr;
    TreeItem hotItem_ = 0;
    bool trackingMouse_ = false;
    std::wstring actionTip_;
    TreeItem renaming_ = 0;
    bool creating_ = false, creatingDirectory_ = false;
    Renderer renderer_;
    Scrollbars scrollbars_;
    TreeModel model_;
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
