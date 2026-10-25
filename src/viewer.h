#pragma once
#include "scrollbars.h"
#include "diff.h"
#include "session.h"
#include "edit_buffer.h"
#include <memory>

namespace poppy {
class Viewer {
public:
    using State = ViewState;
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void show(std::shared_ptr<const Document> document, const ResourceId& path, bool preserve = false);
    void edit(std::shared_ptr<EditBuffer> buffer, const ResourceId& path, bool preserve = false);
    std::shared_ptr<EditBuffer> buffer() const { return edit_; }
    std::function<void()> changed, save, focused, diffLayoutChanged;
    void showDiff(std::shared_ptr<const DiffDocument> document, const ResourceId& path, bool preserve = false);
    void toggleDiffLayout();
    void setDiffLayout(bool inlineView);
    bool diffInline() const { return inlineView_; }
    void message(const std::wstring& text, const ResourceId& path = {});
    State state() const;
    void restore(const State& state);
    size_t documentBytes() const;
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
    void replace(std::string text);
    void edited();
    void moveCaret(WPARAM key, bool shift, bool ctrl);
    void caretPosition(bool reveal = false);
    POINT caretPoint() const;
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
    static constexpr UINT_PTR caretTimer = 0x5056;
    bool caretVisible_ = false;
    std::shared_ptr<const Document> document_;
    std::shared_ptr<EditBuffer> edit_;
    wchar_t highSurrogate_ = 0;
    std::shared_ptr<const DiffDocument> diff_;
    ResourceId path_;
    std::wstring message_ = L"Open a folder to start\n\nSelect a file on the left. Run your AI CLI in the terminal below.";
};
}
