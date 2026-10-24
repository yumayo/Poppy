#pragma once
#include "scrollbars.h"
#include "diff.h"
#include <memory>

namespace poppy {
class Viewer {
public:
    struct State { int top = 0, left = 0; size_t anchor = 0, caret = 0; bool after = false, inlineView = false; };
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void show(std::shared_ptr<const Document> document, const fs::path& path, bool preserve = false);
    void showDiff(std::shared_ptr<const DiffDocument> document, const fs::path& path, bool preserve = false);
    void toggleDiffLayout();
    void message(const std::wstring& text, const fs::path& path = {});
    State state() const { return {top_, left_, anchor_, caret_, after_, inlineView_}; }
    void restore(const State& state);
    bool hasDocument() const { return document_ != nullptr; }
    HWND window() const { return window_; }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void scrollbars();
    void scroll(int vertical, int horizontal);
    size_t position(int x, int y) const;
    void copy();
    void setDiffLayout(bool inlineView);
    RECT layoutButton(bool inlineView) const;
    int visibleRows() const;
    bool splitDiff() const { return diff_ && !inlineView_; }
    int textGutter() const { return diff_ && inlineView_ ? 2 * gutter_ : gutter_; }
    int contentTop() const { return header_ + (diff_ ? header_ : 0); }
    size_t rowCount() const { return diff_ ? diff_->rowCount(inlineView_) : document_->lines.size(); }
    HWND window_ = nullptr;
    Renderer renderer_;
    Scrollbars scrollbars_;
    int cellWidth_ = 8, cellHeight_ = 20, header_ = 34, gutter_ = 66;
    int top_ = 0, left_ = 0;
    size_t anchor_ = 0, caret_ = 0;
    bool selecting_ = false, after_ = false, inlineView_ = false;
    std::shared_ptr<const Document> document_;
    std::shared_ptr<const DiffDocument> diff_;
    fs::path path_;
    std::wstring message_ = L"Open a folder to start\n\nSelect a file on the left. Run your AI CLI in the terminal below.";
};
}
