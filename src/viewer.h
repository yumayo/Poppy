#pragma once
#include "scrollbars.h"
#include "diff.h"
#include "diff_layout.h"
#include "session.h"
#include "edit_buffer.h"
#include "find_bar.h"
#include "text_search.h"
#include "text_layout.h"
#include "editor_scroll.h"
#include "html_preview.h"
#include <memory>

namespace poppy {
class FileDropTarget;
class Viewer {
public:
    using State = ViewState;
    // Text-only popups omit line numbers and display complete wrapped lines.
    void create(HWND parent, HINSTANCE instance, bool textOnly = false);
    // Register on the body window as well as the project's tab strip.
    void setFileDropTarget(FileDropTarget* target);
    void setDpi(UINT dpi);
    void show(std::shared_ptr<const Document> document, const ResourceId& path, bool preserve = false, bool allowHtml = true);
    void showRevision(std::shared_ptr<const Document> document, const DiffDocument& changes, const ResourceId& path, bool preserve = false);
    void edit(std::shared_ptr<EditBuffer> buffer, const ResourceId& path, bool preserve = false);
    void setGitBase(std::shared_ptr<const Document> before);
    void showImage(std::shared_ptr<const ImagePreview> image, const ResourceId& path);
    std::shared_ptr<EditBuffer> buffer() const { return edit_; }
    std::function<void()> changed, save, focused, diffLayoutChanged, statusChanged;
    std::wstring textStatus() const;
    void showDiff(std::shared_ptr<const DiffDocument> document, const ResourceId& path, bool preserve = false);
    void editDiff(std::shared_ptr<EditBuffer> buffer, std::shared_ptr<const Document> before, const ResourceId& path, bool preserve = false);
    // External reloads follow the tail only when the old viewport was at EOF.
    void refreshBuffer(bool externalUpdate = false);
    void toggleDiffLayout();
    void setDiffLayout(bool inlineView);
    bool diffInline() const { return inlineView_; }
    void setWordWrap(bool enabled);
    bool wordWrap() const { return wordWrap_; }
    void message(const std::wstring& text, const ResourceId& path = {});
    void loading(const std::wstring& text, const ResourceId& path = {});
    State state() const;
    void restore(const State& state);
    size_t documentBytes() const;
    bool hasDocument() const { return document_ != nullptr || image_ != nullptr; }
    void find();
    void findNext(bool backwards = false);
    bool findKey(const MSG& message);
    bool selectSearch(size_t begin, size_t end, const std::string& query, bool matchCase);
    bool htmlAvailable() const { return htmlAllowed_ && document_ && !diff_ && isHtmlExtension(path_.extension().native()); }
    void setHtmlPreview(bool enabled);
    bool htmlPreview() const { return html_.active(); }
    HWND window() const { return window_; }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void paintGitMarks();
    RECT contentRect() const;
    void scrollbars(bool followTail = false);
    void scroll(int vertical, int horizontal);
    void extendSelection(int x, int y);
    void scrollSelection();
    void stopSelection();
    void stopLoading();
    void restoreTop(const State& state);
    size_t position(int x, int y, bool* trailing = nullptr, WordRange* word = nullptr) const;
    void copy();
    void cut();
    void replace(std::string text);
    void edited();
    void updateGitLines();
    bool editSelection(size_t& anchor, size_t& caret) const;
    void undo(bool redo);
    void selectAll();
    void moveCaret(WPARAM key, bool shift, bool ctrl);
    void caretPosition(bool reveal = false);
    void findLayout();
    void requestFind(bool navigate = false, bool backwards = false, bool reveal = true);
    void searchSlice();
    void revealMatch();
    void revealRange(TextMatch match);
    POINT caretPoint() const;
    RECT inputRect() const;
    void htmlLayout();
    int visibleRows() const;
    bool splitDiff() const { return diff_ && !inlineView_; }
    bool wrapping() const { return document_ && wordWrap_; }
    size_t diffRow(size_t visualRow) const { return wrapping() ? diffLayout_.row(visualRow).row : visualRow; }
    const DiffFile* stickyFile() const { return diff_ ? diff_->fileForRow(diffRow(static_cast<size_t>(top_)), inlineView_) : nullptr; }
    int textGutter() const { return diff_ && inlineView_ ? 2 * gutter_ : gutter_; }
    int contentTop() const { return findBar_.visible() ? header_ : 0; }
    size_t rowCount() const { return diff_ ? wrapping() ? diffLayout_.rows() : diff_->rowCount(inlineView_) : wrapping() ? textLayout_.rows() : document_->lines.size(); }
    HWND window_ = nullptr;
    FileDropTarget* fileDropTarget_ = nullptr;
    Renderer renderer_;
    HtmlPreview html_;
    ImeComposition ime_;
    FindBar findBar_;
    TextSearch search_;
    bool revealFind_ = false;
    Scrollbars scrollbars_;
    UINT dpi_ = 96;
    int cellWidth_ = 8, cellHeight_ = 20, header_ = 34, gutter_ = 66;
    int top_ = 0, left_ = 0;
    EditorScrollRange scrollRange_;
    size_t anchor_ = 0, caret_ = 0;
    bool selecting_ = false, after_ = false, inlineView_ = true;
    POINT selectionPoint_{};
    bool wordWrap_ = true, layoutDirty_ = true, caretTrailing_ = false;
    bool textOnly_ = false;
    TextLayout textLayout_;
    DiffLayout diffLayout_;
    static constexpr UINT_PTR caretTimer = 0x5056;
    static constexpr UINT_PTR findTimer = 0x5057;
    static constexpr UINT_PTR selectionTimer = 0x5058;
    static constexpr UINT_PTR loadingTimer = 0x5059;
    bool loading_ = false;
    bool htmlAllowed_ = true;
    ULONGLONG loadingDue_ = 0;
    bool caretVisible_ = false;
    std::shared_ptr<const Document> document_;
    std::shared_ptr<const ImagePreview> image_;
    std::shared_ptr<EditBuffer> edit_;
    wchar_t highSurrogate_ = 0;
    std::shared_ptr<const DiffDocument> diff_;
    std::shared_ptr<const Document> diffBefore_;
    std::shared_ptr<const Document> gitBefore_;
    std::vector<bool> gitChangedLines_;
    std::vector<LineChange> gitChangeRanges_;
    std::vector<DiffChange> diffChangeRanges_;
    ResourceId path_;
    std::wstring message_ = L"Open a folder to start\n\nSelect a file on the left. Run your AI CLI in the terminal below.";
};
}
