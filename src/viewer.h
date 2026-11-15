#pragma once
#include <chrono>
#include "scrollbars.h"
#include "diff.h"
#include "diff_layout.h"
#include "session.h"
#include "edit_buffer.h"
#include "column_selection.h"
#include "find_bar.h"
#include "text_search.h"
#include "text_layout.h"
#include "editor_scroll.h"
#include "html_preview.h"
#include "csv_view.h"
#include "file_tail.h"
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
    // Shows a large file's partial document. A newly opened one starts at its
    // first row; a reload follows the end only while the view follows it.
    // A large CSV's window may come with the buffer that owns document,
    // which makes its rows editable until another window is loaded.
    void showTail(std::shared_ptr<const Document> document, const ResourceId& path, bool preserve = false, std::shared_ptr<EditBuffer> buffer = {});
    void showFileWindow(std::shared_ptr<const Document> document, const ResourceId& path, uint64_t requestedOffset, std::shared_ptr<EditBuffer> buffer = {});
    // Redraws an edited window after it was saved, which moved its end.
    void refreshWindow();
    void cancelFileWindowRequest();
    // Exact line numbers for large-file windows. Without an index, windows of
    // files up to maxLineIndexBytes omit line numbers; larger files show the
    // byte offset of each line instead.
    void setLineIndex(std::shared_ptr<const FileLineIndex> index, bool counting);
    const Document* document() const { return document_.get(); }
    bool tailPreview() const { return document_ && document_->tail && !diff_; }
    // An edited window holds the only copy of its edits, so it neither loads
    // other windows nor follows the file until it is saved or undone.
    bool windowLocked() const { return tailPreview() && edit_ && (edit_->dirty() || edit_->saving); }
    bool tailPaused() const { return tailPreview() && (windowLocked() || pendingFileOffset_ || document_->tailEndOffset < document_->fileBytes || !atTail()); }
    // The view shows the end of its file, so reloads follow appended text.
    bool followsTail() const { return tailPreview() ? !tailPaused() : document_ && !diff_ && !html_.active() && atTail(); }
    void showRevision(std::shared_ptr<const Document> document, const DiffDocument& changes, const ResourceId& path, bool preserve = false);
    void edit(std::shared_ptr<EditBuffer> buffer, const ResourceId& path, bool preserve = false);
    void setGitBase(std::shared_ptr<const Document> before);
    void showImage(std::shared_ptr<const ImagePreview> image, const ResourceId& path);
    std::shared_ptr<EditBuffer> buffer() const { return edit_; }
    std::function<void()> changed, save, focused, diffLayoutChanged, statusChanged;
    std::function<void()> tailRequested;
    std::function<void(uint64_t)> fileWindowRequested;
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
    // state() plus, for a large file's table, its cells by file byte, which
    // restore() places in whichever window of the file is loaded.
    State sessionState() const;
    void restore(const State& state);
    size_t documentBytes() const;
    bool hasDocument() const { return document_ != nullptr || image_ != nullptr; }
    std::optional<std::wstring> searchSeed() const;
    void find();
    void findNext(bool backwards = false);
    bool findKey(const MSG& message);
    bool selectSearch(size_t begin, size_t end, const std::string& query, bool matchCase);
    bool htmlAvailable() const { return htmlAllowed_ && document_ && !diff_ && isHtmlExtension(path_.extension().native()); }
    void setHtmlPreview(bool enabled);
    bool htmlPreview() const { return html_.active(); }
    // A large file's window is shown as a read-only table of its records.
    bool csvAvailable() const { return !textOnly_ && (htmlAllowed_ || tailPreview()) && document_ && !diff_ && isCsvExtension(path_.extension().native()); }
    void setCsvTable(bool enabled);
    bool csvTable() const { return csv_.active(); }
    HWND window() const { return window_; }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    // Table cells of a large file's window, kept by file byte so they survive
    // loading another window. A move places the caret rows after the
    // requested offset's record once that window arrives.
    // Cells outside the window keep their record's byte, row name and value.
    // The top row's byte keeps the rows on screen in place.
    struct CsvCarry {
        uint64_t anchor = 0, caret = 0, record = 0, top = 0; size_t anchorColumn = 0, column = 0; int left = 0; bool move = false, extend = false;
        std::wstring name, value;
    };
    std::optional<CsvCarry> present(std::shared_ptr<const Document> document, const ResourceId& path, bool preserve, bool allowHtml, std::shared_ptr<EditBuffer> buffer = {});
    bool loadsWindows() const { return fileWindowRequested && !windowLocked(); }
    // Keeps file byte mapping in step with an edited window's text.
    void windowEdited();
    CsvCarry captureCsv() const;
    void placeCsv(const CsvCarry& carry);
    CsvView::Mark csvMark(uint64_t byte, size_t column) const;
    void scrollTable(int vertical);
    bool moveTableCaret(int64_t rows, bool extend);
    void loadTableRows(int direction);
    bool atTail() const { return csv_.active() ? csv_.atBottom() : scrollRange_.atTail(top_); }
    uint64_t bomBytes() const { return document_->encoding.compare(0, 6, "UTF-16") == 0 ? 2u : 3u; }
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void paintGitMarks();
    RECT contentRect() const;
    void scrollbars(bool followTail = false);
    void scroll(int vertical, int horizontal);
    void seekFileByte(uint64_t offset);
    void requestFileWindow(uint64_t offset, int rows = 0, std::optional<CsvCarry> carry = std::nullopt);
    void indexFileBytes();
    uint64_t fileByteAt(size_t offset) const;
    size_t fileTextAt(uint64_t offset) const;
    uint64_t fileRowByte(int row) const;
    std::optional<uint64_t> lineBase() const;
    std::wstring lineLabel(size_t line, const std::optional<uint64_t>& base) const;
    void updateGutter();
    void extendSelection(int x, int y);
    void extendColumns(int x, int y);
    void clearColumns();
    void syncColumns();
    bool hasSelection() const;
    void replaceColumnSelection(std::string_view text, bool paste = false, ColumnErase erase = ColumnErase::None);
    void scrollSelection();
    void stopSelection();
    void stopLoading();
    void restoreTop(const State& state);
    size_t position(int x, int y, bool* trailing = nullptr, WordRange* word = nullptr) const;
    void copy();
    void cut();
    void replace(std::string text, bool paste = false);
    void outdent();
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
    void searchingStatus();
    void revealMatch();
    void revealRange(TextMatch match);
    POINT caretPoint() const;
    POINT caretPoint(size_t offset, bool trailing) const;
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
    CsvView csv_;
    ImeComposition ime_;
    FindBar findBar_;
    TextSearch search_;
    bool revealFind_ = false;
    // When the current search started. "Searching…" appears only after a
    // second, so quick searches and jumps keep the previous status.
    std::chrono::steady_clock::time_point findStarted_{};
    Scrollbars scrollbars_;
    UINT dpi_ = 96;
    int cellWidth_ = 8, cellHeight_ = 20, header_ = 34, gutter_ = 66, baseGutter_ = 66;
    int top_ = 0, left_ = 0;
    EditorScrollRange scrollRange_;
    size_t anchor_ = 0, caret_ = 0;
    bool selecting_ = false, after_ = false, inlineView_ = true;
    bool columnDragging_ = false, columnCaretAtEnd_ = true;
    size_t columnStartRow_ = 0, columnActive_ = 0;
    int columnStart_ = 0;
    std::vector<ColumnRange> columnRanges_;
    POINT selectionPoint_{};
    bool wordWrap_ = true, layoutDirty_ = true, caretTrailing_ = false;
    bool textOnly_ = false;
    bool tailWasPaused_ = false;
    std::optional<uint64_t> pendingFileOffset_;
    std::optional<CsvCarry> csvCarry_;
    // The cells last placed in the table, whose bytes stand for any selected
    // cell outside the loaded window.
    CsvCarry csvMarks_;
    int pendingFileRows_ = 0;
    // UTF-16 positions use original code units, while the displayed text is UTF-8.
    std::vector<uint32_t> fileLineBytes_;
    std::shared_ptr<const FileLineIndex> lineIndex_;
    bool lineCounting_ = false;
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
