#pragma once
#include "render.h"
#include "tree_messages.h"

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
    void expand(TreeItem id, bool expanded);
    void select(TreeItem id, bool activate = true);
    TreeItem child(TreeItem parent = 0) const { return model_.child(parent); }
    TreeItem next(TreeItem id) const { return model_.next(id); }
    TreeItem selection() const { return model_.selection(); }
    std::function<void(TreeItem)> activated;
    std::function<void(TreeItem, bool)> expanded;
    std::function<void(uintptr_t)> removed;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void layout();
    void changed();
    void ensureVisible();
    int row(TreeItem id) const;
    TreeItem hit(int y) const;
    void scroll(int vertical, int horizontal = 0);
    void release(const std::vector<std::pair<TreeItem, uintptr_t>>& items);
    int page() const;
    HWND window_ = nullptr;
    Renderer renderer_;
    TreeModel model_;
    std::unordered_map<TreeItem, int> widths_;
    UINT dpi_ = 96;
    int rowHeight_ = 26, indent_ = 18, top_ = 0, left_ = 0, wheel_ = 0, batch_ = 0;
    bool layingOut_ = false;
    std::wstring search_;
    ULONGLONG searchTime_ = 0;
};
}
