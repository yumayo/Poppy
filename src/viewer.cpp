#include "viewer.h"
#include "file_drop.h"
#include "selection_scroll.h"
#include "change_marks.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <imm.h>

namespace poppy {
namespace {
constexpr std::array<Color, 6> styles{foreground, rgb(191,151,220), rgb(151,195,121), rgb(107,129,145), rgb(224,192,128), rgb(231,112,118)};
constexpr Color removedBackground = rgb(62, 39, 45), addedBackground = rgb(36, 57, 44);
}
void Viewer::create(HWND parent, HINSTANCE instance, bool textOnly) {
    textOnly_ = textOnly;
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyViewer"; cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    cls.style = CS_DBLCLKS;
    RegisterClassW(&cls);
    window_ = CreateWindowExW(0, cls.lpszClassName, L"Code viewer", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, instance, this);
    csv_.attach(window_, renderer_, scrollbars_);
    csv_.changed = [this] {
        layoutDirty_ = true; updateGitLines();
        if (changed) changed();
        if (statusChanged) statusChanged();
    };
    csv_.save = [this] { if (save) save(); };
    csv_.statusChanged = [this] { if (statusChanged) statusChanged(); };
    findBar_.create(window_, instance);
    findBar_.changed = [this] { requestFind(); };
    findBar_.navigate = [this](bool backwards) { findNext(backwards); };
    findBar_.focused = [this] { if (focused) focused(); };
    findBar_.closed = [this] {
        KillTimer(window_, findTimer); search_.query("");
        scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
    };
    html_.changed = [this] { InvalidateRect(window_, nullptr, FALSE); };
    html_.focused = [this] { if (focused) focused(); };
    setDpi(GetDpiForWindow(parent));
}
void Viewer::setFileDropTarget(FileDropTarget* target) {
    if (target == fileDropTarget_) return;
    if (fileDropTarget_) {
        RevokeDragDrop(window_); fileDropTarget_->Release(); fileDropTarget_ = nullptr;
    }
    if (!target) return;
    const auto result = RegisterDragDrop(window_, target);
    if (FAILED(result)) throw std::runtime_error("Cannot register editor body file drops (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ").");
    target->AddRef(); fileDropTarget_ = target;
}
void Viewer::setDpi(UINT dpi) {
    stopSelection(); clearColumns();
    dpi_ = dpi;
    renderer_.setDpi(dpi);
    cellWidth_ = renderer_.cellWidth(); cellHeight_ = renderer_.textHeight() + MulDiv(3, dpi, 96);
    header_ = MulDiv(34, dpi, 96); gutter_ = MulDiv(textOnly_ ? 12 : 70, dpi, 96);
    findBar_.setDpi(dpi);
    csv_.setDpi(dpi);
    scrollbars(); findLayout(); htmlLayout(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::show(std::shared_ptr<const Document> d, const ResourceId& path, bool preserve, bool allowHtml) {
    const bool preview = preserve && path_ == path && htmlAvailable() ? html_.active() : true;
    const bool table = preserve && path_ == path && csvAvailable() ? csv_.active() : true;
    const auto previous = preserve && document_ ? std::optional<State>(state()) : std::nullopt;
    csv_.hide(!preserve || path_ != path);
    html_.hide();
    htmlAllowed_ = allowHtml;
    stopLoading();
    stopSelection(); clearColumns();
    ime_.cancel(window_);
    image_.reset(); renderer_.setImage({});
    edit_.reset(); diffBefore_.reset(); gitBefore_.reset(); gitChangedLines_ = {}; gitChangeRanges_ = {}; highSurrogate_ = 0;
    diffLayout_ = {}; layoutDirty_ = true; caretTrailing_ = false;
    diff_.reset(); diffChangeRanges_ = {}; document_ = std::move(d); path_ = path; message_.clear();
    if (!preserve) { top_ = left_ = 0; anchor_ = caret_ = 0; after_ = false; }
    else { anchor_ = std::min(anchor_, document_->text.size()); caret_ = std::min(caret_, document_->text.size()); }
    scrollbars(); findLayout(); requestFind(false, false, false); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
    if (previous) restore(*previous);
    if (preview) setHtmlPreview(true);
    if (table) setCsvTable(true);
    if (previous && csv_.active()) restore(*previous);
    if (statusChanged) statusChanged();
}
void Viewer::showRevision(std::shared_ptr<const Document> d, const DiffDocument& changes, const ResourceId& path, bool preserve) {
    show(std::move(d), path, preserve, false);
    gitChangeRanges_ = revisionAddedLines(changes, document_->lines.size());
    gitChangedLines_.assign(document_->lines.size(), false);
    for (const auto& range : gitChangeRanges_)
        std::fill(gitChangedLines_.begin() + range.begin, gitChangedLines_.begin() + range.end, true);
    InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::edit(std::shared_ptr<EditBuffer> buffer, const ResourceId& path, bool preserve) {
    const auto before = edit_ == buffer && path_ == path ? gitBefore_ : nullptr;
    show(std::shared_ptr<const Document>(buffer, &buffer->document), path, preserve);
    edit_ = std::move(buffer); caretPosition();
    if (csv_.active()) csv_.buffer(edit_);
    setGitBase(before);
}
void Viewer::setGitBase(std::shared_ptr<const Document> before) {
    gitBefore_ = std::move(before); updateGitLines();
    InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::updateGitLines() {
    gitChangeRanges_.clear();
    gitChangedLines_ = gitBefore_ && document_ && !diff_ ? changedWorkingLines(*gitBefore_, *document_, &gitChangeRanges_) : std::vector<bool>{};
}
void Viewer::showDiff(std::shared_ptr<const DiffDocument> d, const ResourceId& path, bool preserve) {
    csv_.hide(true);
    html_.hide();
    stopLoading();
    stopSelection(); clearColumns();
    ime_.cancel(window_);
    image_.reset(); renderer_.setImage({});
    const auto previous = preserve && document_ ? std::optional<State>(state()) : std::nullopt;
    edit_.reset(); diffBefore_.reset(); gitBefore_.reset(); gitChangedLines_ = {}; gitChangeRanges_ = {}; highSurrogate_ = 0;
    textLayout_ = {}; layoutDirty_ = true; caretTrailing_ = false;
    // Share the patch snapshot; both panes only keep references to its lines.
    document_ = std::shared_ptr<const Document>(d, &d->patch); diff_ = std::move(d); path_ = path; message_.clear();
    diffChangeRanges_ = changedDiffRows(*diff_, inlineView_);
    if (!preserve) { top_ = left_ = 0; anchor_ = caret_ = 0; after_ = false; }
    else { anchor_ = std::min(anchor_, document_->text.size()); caret_ = std::min(caret_, document_->text.size()); }
    scrollbars(); findLayout(); requestFind(false, false, false); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
    if (previous) restore(*previous);
    if (statusChanged) statusChanged();
}
void Viewer::editDiff(std::shared_ptr<EditBuffer> buffer, std::shared_ptr<const Document> before, const ResourceId& path, bool preserve) {
    const auto anchor = preserve && diff_ && edit_ == buffer && after_ ? diff_->workingOffset(anchor_) : std::nullopt;
    const auto caret = preserve && diff_ && edit_ == buffer && after_ ? diff_->workingOffset(caret_) : std::nullopt;
    auto comparison = std::make_shared<DiffDocument>(workingDiff(*before, buffer->document));
    showDiff(std::move(comparison), path, preserve);
    edit_ = std::move(buffer); diffBefore_ = std::move(before);
    if (!preserve) { anchor_ = caret_ = diff_->patchOffset(0); after_ = true; }
    else if (anchor && caret) { anchor_ = diff_->patchOffset(*anchor); caret_ = diff_->patchOffset(*caret); after_ = true; }
    caretPosition();
}
void Viewer::refreshBuffer(bool externalUpdate) {
    if (!edit_) return;
    stopSelection(); clearColumns();
    if (csv_.active()) {
        csv_.refresh(); updateGitLines(); layoutDirty_ = true;
        if (statusChanged) statusChanged();
        return;
    }
    // The shared document has already changed; use the previous layout's
    // range, including padding, before rebuilding wrapped rows.
    const bool followTail = externalUpdate && !diff_ && !html_.active() &&
        scrollRange_.atTail(top_);
    if (diff_) {
        const auto previous = state();
        const auto anchor = diff_->workingOffset(anchor_), caret = diff_->workingOffset(caret_);
        auto comparison = std::make_shared<DiffDocument>(workingDiff(*diffBefore_, edit_->document));
        document_ = std::shared_ptr<const Document>(comparison, &comparison->patch); diff_ = std::move(comparison);
        diffChangeRanges_ = changedDiffRows(*diff_, inlineView_);
        anchor_ = anchor ? diff_->patchOffset(*anchor) : std::min(anchor_, document_->text.size());
        caret_ = caret ? diff_->patchOffset(*caret) : std::min(caret_, document_->text.size());
        layoutDirty_ = true; scrollbars(); restoreTop(previous);
    } else {
        anchor_ = std::min(anchor_, document_->text.size()); caret_ = std::min(caret_, document_->text.size());
        updateGitLines();
    }
    layoutDirty_ = true; requestFind(false, false, false); scrollbars(followTail); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::message(const std::wstring& text, const ResourceId& path) {
    csv_.hide(true);
    html_.hide();
    stopLoading();
    stopSelection(); clearColumns();
    ime_.cancel(window_);
    image_.reset(); renderer_.setImage({});
    findBar_.close(); KillTimer(window_, findTimer); search_.query("");
    edit_.reset(); diffBefore_.reset(); gitBefore_.reset(); gitChangedLines_ = {}; gitChangeRanges_ = {}; highSurrogate_ = 0;
    textLayout_ = {}; diffLayout_ = {}; layoutDirty_ = true; caretTrailing_ = false;
    diff_.reset(); diffChangeRanges_ = {}; document_.reset(); path_ = path; message_ = text; top_ = left_ = 0; anchor_ = caret_ = 0; after_ = false;
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
    if (statusChanged) statusChanged();
}
void Viewer::loading(const std::wstring& text, const ResourceId& path) {
    // Repository discovery and diff loading are one continuous wait for a tab.
    if (loading_ && path_ == path) {
        message_ = text;
        if (!loadingDue_) InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    // Release the previous document immediately so it cannot still be edited.
    message(L"", path);
    message_ = text; loading_ = true; loadingDue_ = GetTickCount64() + 1000;
    SetTimer(window_, loadingTimer, 1000, nullptr);
}
void Viewer::stopLoading() {
    if (window_) KillTimer(window_, loadingTimer);
    loading_ = false; loadingDue_ = 0;
}
void Viewer::showImage(std::shared_ptr<const ImagePreview> image, const ResourceId& path) {
    message(L"", path);
    image_ = std::move(image); renderer_.setImage(image_);
    InvalidateRect(window_, nullptr, FALSE);
    if (statusChanged) statusChanged();
}
void Viewer::htmlLayout() {
    html_.layout(Scrollbars::contentRect(window_));
}
void Viewer::setHtmlPreview(bool enabled) {
    if (!htmlAvailable() || enabled == html_.active()) return;
    stopSelection(); clearColumns(); ime_.cancel(window_); findBar_.close();
    if (enabled) html_.show(window_, path_, {});
    else html_.hide();
    scrollbars(); findLayout(); htmlLayout(); caretPosition();
    InvalidateRect(window_, nullptr, FALSE);
    if (!enabled) SetFocus(window_);
    if (statusChanged) statusChanged();
}
void Viewer::setCsvTable(bool enabled) {
    if (!csvAvailable() || enabled == csv_.active()) return;
    const auto previous = state();
    stopSelection(); clearColumns(); ime_.cancel(window_); findBar_.close();
    if (enabled) {
        csv_.show(document_, true); csv_.buffer(edit_); csv_.restore(previous);
        KillTimer(window_, caretTimer); caretVisible_ = false;
    } else {
        csv_.hide(); layoutDirty_ = true;
        auto source = previous; source.left = 0;
        scrollbars(); restore(source);
    }
    InvalidateRect(window_, nullptr, FALSE);
    if (statusChanged) statusChanged();
}
std::wstring Viewer::textStatus() const {
    if (image_) return std::to_wstring(image_->width) + L" × " + std::to_wstring(image_->height) + L" px";
    // A Git patch does not describe the source files' encoding or line endings.
    if (!document_ || diff_) return {};
    const auto newline = document_->text.find('\n');
    const bool crlf = newline != std::string::npos && newline > 0 && document_->text[newline - 1] == '\r';
    return (csv_.active() ? csv_.status() + L"  ·  " : L"") + wide(document_->encoding) + (crlf ? L"  ·  CRLF" : L"  ·  LF");
}
Viewer::State Viewer::state() const {
    if (csv_.active()) return csv_.state();
    if (wrapping() && diff_ && diffLayout_.rows()) {
        const auto display = diffLayout_.row(static_cast<size_t>(top_));
        const auto change = diff_->displayRow(display.row, inlineView_);
        bool after = inlineView_ ? change.after.number != 0 : after_;
        const auto source = after ? change.after : change.before;
        // Padding on a shorter split side still needs a source position on
        // the longer side so resizing or switching layouts keeps this row.
        if (!source.number || display.continuation >= wrappedLineRows(diff_->line(source), diffLayout_.columns())) after = !after;
        const auto row = diffLayout_.textRow(*diff_, static_cast<size_t>(top_), after);
        return {static_cast<int>(diff_->mapRow(display.row, inlineView_, true, after)), 0, anchor_, caret_, after_, row.begin};
    }
    if (wrapping() && !diff_ && textLayout_.rows()) {
        const auto row = textLayout_.row(*document_, static_cast<size_t>(top_));
        return {static_cast<int>(row.line), 0, anchor_, caret_, after_, row.begin};
    }
    // Inline row indices retain both sides without storing a layout per tab.
    const int top = diff_ ? static_cast<int>(diff_->mapRow(static_cast<size_t>(top_), inlineView_, true, after_)) : top_;
    return {top, left_, anchor_, caret_, after_};
}
void Viewer::restoreTop(const State& state) {
    if (!document_) return;
    top_ = diff_ ? static_cast<int>(diff_->mapRow(static_cast<size_t>(std::max(0, state.top)), true, inlineView_, state.after)) : state.top;
    if (wrapping() && diff_ && diff_->rowCount(true)) {
        const auto row = diff_->displayRow(std::min(static_cast<size_t>(std::max(0, state.top)), diff_->rowCount(true) - 1), true);
        top_ = static_cast<int>(diffLayout_.rowForOffset(*diff_, static_cast<size_t>(top_), state.wrapOffset, row.after.number != 0));
    } else if (wrapping() && !diff_ && !document_->lines.empty()) {
        const size_t line = std::min(static_cast<size_t>(std::max(0, state.top)), document_->lines.size() - 1);
        top_ = static_cast<int>(textLayout_.rowForOffset(*document_, document_->lines[line] + std::min(state.wrapOffset, document_->line(line).size())));
    }
}
void Viewer::restore(const State& state) {
    if (!document_) return;
    stopSelection(); clearColumns();
    if (csv_.active()) { csv_.restore(state); return; }
    restoreTop(state);
    caretTrailing_ = false;
    left_ = state.left; after_ = state.after;
    anchor_ = std::min(state.anchor, document_->text.size()); caret_ = std::min(state.caret, document_->text.size());
    if (diff_ && edit_ && !anchor_ && !caret_) { anchor_ = caret_ = diff_->patchOffset(0); after_ = true; }
    requestFind(false, false, false);
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::setWordWrap(bool enabled) {
    if (wordWrap_ == enabled) return;
    stopSelection(); clearColumns();
    const auto previous = state();
    wordWrap_ = enabled; layoutDirty_ = true;
    scrollbars(); findLayout();
    if (document_) { restore(previous); if (edit_ && GetFocus() == window_) caretPosition(true); }
    else if (window_) InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::toggleDiffLayout() { if (diff_) setDiffLayout(!inlineView_); }
void Viewer::setDiffLayout(bool inlineView) {
    if (inlineView_ == inlineView) return;
    stopSelection(); clearColumns();
    const auto previous = state();
    inlineView_ = inlineView; layoutDirty_ = true;
    if (diff_) diffChangeRanges_ = changedDiffRows(*diff_, inlineView_);
    stopSelection();
    scrollbars();
    if (diff_) { restoreTop(previous); scrollbars(); }
    caretPosition(); InvalidateRect(window_, nullptr, FALSE);
    if (diffLayoutChanged) diffLayoutChanged();
}
RECT Viewer::contentRect() const {
    auto rect = Scrollbars::contentRect(window_);
    // Reserve the same strip in source and diff views, including before an
    // asynchronous Git base arrives, so loading it never reflows the source.
    if (document_ && !html_.active()) rect.right = std::max(0L, rect.right - MulDiv(4, dpi_, 96));
    return rect;
}
int Viewer::visibleRows() const { const auto r = contentRect(); return std::max(1, (static_cast<int>(r.bottom) - contentTop()) / cellHeight_); }
void Viewer::scrollbars(bool followTail) {
    if (!window_) return;
    if (csv_.active()) { csv_.layout(); return; }
    if (html_.active()) {
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 1, 0, 0};
        SetScrollInfo(window_, SB_VERT, &info, FALSE); SetScrollInfo(window_, SB_HORZ, &info, FALSE);
        return;
    }
    const int cols = document_ ? static_cast<int>(diff_ ? diff_->maxColumns : document_->maxColumns) : 1;
    std::optional<State> reflow;
    // Each bar can change the space available to the other.
    for (int pass = 0; pass < 3; ++pass) {
        const auto r = contentRect();
        const int width = splitDiff() ? (static_cast<int>(r.right) - 1) / 2 : static_cast<int>(r.right);
        const int pageRows = visibleRows(), pageCols = std::max(1, (width - textGutter()) / cellWidth_);
        if (wrapping()) {
            // Leave room for the caret after the final cell of a full row.
            const int columns = std::max(1, (width - textGutter() - 2) / cellWidth_);
            if (layoutDirty_ || (diff_ ? diffLayout_.columns() : textLayout_.columns()) != columns) {
                if (!layoutDirty_ && !reflow && rowCount()) reflow = state();
                if (diff_) diffLayout_.build(*diff_, inlineView_, columns);
                else textLayout_.build(*document_, columns, textOnly_ ? std::numeric_limits<int>::max() : static_cast<int>(maxLineColumns));
                layoutDirty_ = false;
                if (reflow) restoreTop(*reflow);
            }
        }
        const int rows = document_ ? static_cast<int>(rowCount()) : 0;
        // All Changes keeps a filename above the last source row. Text-only
        // message popups retain their compact scroll range.
        const int pinned = diff_ && rows && diff_->fileForRow(diffRow(rows - 1), inlineView_) ? 1 : 0;
        scrollRange_ = {rows, pageRows, document_ && !textOnly_, pinned};
        top_ = followTail ? scrollRange_.maximumTop() : std::clamp(top_, 0, scrollRange_.maximumTop());
        left_ = std::clamp(left_, 0, std::max(0, cols - pageCols));
        if (wrapping()) left_ = 0;
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0, scrollRange_.totalRows() - 1, static_cast<UINT>(pageRows), top_, 0}; SetScrollInfo(window_, SB_VERT, &vertical, FALSE);
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0, wrapping() ? 0 : cols - 1, static_cast<UINT>(pageCols), left_, 0}; SetScrollInfo(window_, SB_HORZ, &horizontal, FALSE);
    }
}
void Viewer::scroll(int vertical, int horizontal) {
    revealFind_ = false;
    top_ += vertical; left_ += horizontal; scrollbars(); caretPosition();
    InvalidateRect(window_, nullptr, FALSE);
    // WM_PAINT has lower priority than mouse input. Present this position
    // before handling another move, including while a thumb is dragged quickly.
    UpdateWindow(window_);
}
void Viewer::extendSelection(int x, int y) {
    selectionPoint_ = {x, y};
    const auto rect = contentRect();
    const int top = contentTop() + (stickyFile() ? cellHeight_ : 0);
    if (rect.bottom <= top) return;
    if (columnDragging_) { extendColumns(x, std::clamp(y, top, static_cast<int>(rect.bottom) - 1)); return; }
    // Hit-test the visible edge; the timer brings further rows into view.
    caret_ = position(x, std::clamp(y, top, static_cast<int>(rect.bottom) - 1), &caretTrailing_);
    caretPosition(); InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_);
}
void Viewer::clearColumns() {
    if (!columnRanges_.empty()) anchor_ = caret_;
    columnRanges_.clear();
}
void Viewer::syncColumns() {
    if (columnRanges_.empty()) return;
    columnActive_ = std::min(columnActive_, columnRanges_.size() - 1);
    const auto& range = columnRanges_[columnActive_];
    anchor_ = caret_ = columnCaretAtEnd_ ? range.end : range.begin;
    caretTrailing_ = columnCaretAtEnd_ && range.trailing;
}
bool Viewer::hasSelection() const {
    return columnRanges_.empty() ? anchor_ != caret_ : std::any_of(columnRanges_.begin(), columnRanges_.end(),
        [](const ColumnRange& range) { return range.begin != range.end || range.virtualWidth; });
}
void Viewer::extendColumns(int x, int y) {
    const size_t lastRow = std::min(rowCount() - 1, static_cast<size_t>(std::max(0, (y - contentTop()) / cellHeight_ + top_)));
    const int origin = textGutter() + (splitDiff() && after_ ? contentRect().right / 2 + 1 : 0);
    if (splitDiff()) x = std::min(x, static_cast<int>(after_ ? contentRect().right : contentRect().right / 2));
    const int column = std::clamp((x - origin + cellWidth_ / 2) / cellWidth_ + left_, 0, static_cast<int>(maxLineColumns));
    columnRanges_.clear();
    for (size_t row = std::min(columnStartRow_, lastRow); row <= std::max(columnStartRow_, lastRow); ++row) {
        WrappedRow wrapped;
        std::string_view line; size_t offset = 0;
        if (diff_) {
            const auto change = diff_->displayRow(diffRow(row), inlineView_);
            const auto source = after_ ? change.after : change.before;
            if (change.heading || !source.number) continue;
            line = diff_->line(source); offset = diff_->offset(source);
            if (wrapping()) {
                if (diffLayout_.row(row).continuation >= wrappedLineRows(line, diffLayout_.columns())) continue;
                wrapped = diffLayout_.textRow(*diff_, row, after_);
            }
        } else {
            if (wrapping()) wrapped = textLayout_.row(*document_, row);
            const size_t logical = wrapping() ? wrapped.line : row;
            line = document_->line(logical); offset = document_->lines[logical];
        }
        if (!wrapping()) wrapped.end = line.size();
        auto range = columnRange(line, wrapped, columnStart_, column);
        range.begin += offset; range.end += offset;
        columnRanges_.push_back(range);
    }
    columnActive_ = lastRow < columnStartRow_ || columnRanges_.empty() ? 0 : columnRanges_.size() - 1;
    columnCaretAtEnd_ = column >= columnStart_;
    syncColumns(); caretPosition(); InvalidateRect(window_, nullptr, FALSE); UpdateWindow(window_);
}
void Viewer::scrollSelection() {
    if (!selecting_ || GetCapture() != window_) { stopSelection(); return; }
    const auto rect = contentRect();
    const int rows = selectionScrollRows(selectionPoint_.y, contentTop() + (stickyFile() ? cellHeight_ : 0), rect.bottom, cellHeight_);
    if (!rows) return;
    const int previous = top_;
    top_ += rows; scrollbars();
    if (top_ != previous) extendSelection(selectionPoint_.x, selectionPoint_.y);
}
void Viewer::stopSelection() {
    if (window_) KillTimer(window_, selectionTimer);
    if (!selecting_) return;
    selecting_ = false; columnDragging_ = false;
    if (GetCapture() == window_) ReleaseCapture();
}
size_t Viewer::position(int x, int y, bool* trailing, WordRange* word) const {
    if (trailing) *trailing = false;
    if (!document_ || !rowCount()) return 0;
    if (stickyFile()) y = std::max(y, contentTop() + cellHeight_);
    const size_t visualRow = std::min(rowCount() - 1, static_cast<size_t>(std::max(0, (y - contentTop()) / cellHeight_ + top_)));
    int origin = textGutter();
    WrappedRow wrapped;
    std::string_view line; size_t offset = 0;
    if (diff_) {
        const size_t row = diffRow(visualRow);
        const auto rect = contentRect();
        if (splitDiff()) {
            origin += after_ ? static_cast<int>(rect.right) / 2 + 1 : 0;
            x = std::min(x, static_cast<int>(after_ ? rect.right : rect.right / 2));
        }
        auto change = diff_->displayRow(row, inlineView_);
        const bool after = inlineView_ && !(edit_ && after_) ? change.after.number != 0 : after_;
        auto source = after ? change.after : change.before;
        if (!source.number) {
            // Empty alignment cells map to the next source line's start, or
            // the preceding line's end when this side has no more content.
            for (size_t next = row + 1; next < diff_->rowCount(inlineView_); ++next) {
                change = diff_->displayRow(next, inlineView_); source = after_ ? change.after : change.before;
                if (source.number) return diff_->offset(source);
            }
            for (size_t previous = row; previous > 0; --previous) {
                change = diff_->displayRow(previous - 1, inlineView_); source = after_ ? change.after : change.before;
                if (source.number) return diff_->offset(source) + diff_->line(source).size();
            }
            return 0;
        }
        line = diff_->line(source); offset = diff_->offset(source);
        if (wrapping()) wrapped = diffLayout_.textRow(*diff_, visualRow, after);
    } else {
        if (wrapping()) wrapped = textLayout_.row(*document_, visualRow);
        const size_t logicalRow = wrapping() ? wrapped.line : visualRow;
        line = document_->line(logicalRow); offset = document_->lines[logicalRow];
    }
    // A double-click targets the character under the pointer, including its
    // right half, rather than rounding to the nearest insertion point.
    const int target = std::max(0, (x - origin + (word ? 0 : cellWidth_ / 2)) / cellWidth_ + left_) + (wrapping() ? wrapped.column : 0);
    int col = wrapping() ? wrapped.column : 0; size_t p = wrapping() ? wrapped.begin : 0;
    const size_t end = wrapping() ? wrapped.end : line.size();
    while (p < end && col < target) {
        const size_t before = p; const char32_t c = decodeUtf8(line, p);
        const int width = c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c));
        if (col + width > target) { p = before; break; } col += width;
    }
    if (word) {
        const auto range = p < end ? wordRange(line, p) : WordRange{p, p};
        *word = {offset + range.begin, offset + range.end};
    }
    if (trailing && wrapping()) *trailing = p == wrapped.end && p < line.size();
    return offset + p;
}
void Viewer::copy() {
    if (!document_ || !hasSelection()) return;
    if (!columnRanges_.empty()) {
        const auto text = selectedColumns(document_->text, columnRanges_);
        if (!text || !copyText(window_, wide(*text))) MessageBeep(MB_ICONWARNING);
        return;
    }
    const size_t start = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
    size_t anchor = 0, caret = 0;
    if (diff_ && editSelection(anchor, caret)) copyText(window_, wide(std::string_view(edit_->document.text).substr(std::min(anchor, caret), std::max(anchor, caret) - std::min(anchor, caret))));
    else if (diff_) copyText(window_, wide(diff_->selectedText(after_, start, end, inlineView_)));
    else copyText(window_, wide(std::string_view(document_->text).substr(start, end - start)));
}
void Viewer::cut() {
    if (!columnRanges_.empty()) {
        size_t anchor = 0, caret = 0;
        if (!editSelection(anchor, caret) || !hasSelection()) return;
        const auto text = selectedColumns(document_->text, columnRanges_);
        if (text && copyText(window_, wide(*text))) replaceColumnSelection("");
        else MessageBeep(MB_ICONWARNING);
        return;
    }
    size_t anchor = 0, caret = 0;
    if (!editSelection(anchor, caret)) return;
    const auto range = edit_->cutRange(anchor, caret);
    if (range.begin == range.end) return;
    if (!copyText(window_, wide(range.text)) || !edit_->replace(range.begin, range.end, "")) { MessageBeep(MB_ICONWARNING); return; }
    anchor_ = caret_ = range.begin; edited();
}
void Viewer::findLayout() {
    if (findBar_.window()) findBar_.layout(0, Scrollbars::contentRect(window_).right, header_);
}
std::optional<std::wstring> Viewer::searchSeed() const {
    if (csv_.active()) return csv_.searchSeed();
    if (!document_ || html_.active() || !columnRanges_.empty() || anchor_ == caret_) return {};
    const size_t begin = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
    if (end - begin > 2048) return {};
    const auto selected = diff_ ? diff_->selectedText(after_, begin, end, inlineView_) : document_->text.substr(begin, end - begin);
    if (selected.empty() || selected.find_first_of("\r\n") != std::string::npos) return {};
    return wide(selected);
}
void Viewer::find() {
    if (!document_) return;
    const auto seed = searchSeed();
    stopSelection(); clearColumns();
    if (csv_.active()) setCsvTable(false);
    if (html_.active()) setHtmlPreview(false);
    findBar_.show(seed); scrollbars(); findLayout(); requestFind();
    InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::findNext(bool backwards) {
    if (!document_) return;
    if (!findBar_.visible()) find();
    requestFind(true, backwards);
}
bool Viewer::findKey(const MSG& message) {
    if (html_.active()) return false;
    if (csv_.active() && csv_.composing() && message.hwnd == window_) return false;
    if (ime_.active() && message.hwnd == window_) return false;
    if (message.message != WM_KEYDOWN || !document_) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && !shift && !(GetKeyState(VK_MENU) & 0x8000) && message.wParam == 'F') { find(); return true; }
    if (message.hwnd == findBar_.window()) return false; // Let the input's IME handle these keys first.
    if (!ctrl && message.wParam == VK_F3) { findNext(shift); return true; }
    if (message.wParam == VK_ESCAPE && findBar_.visible()) { findBar_.close(); return true; }
    return false;
}
void Viewer::requestFind(bool navigate, bool backwards, bool reveal) {
    KillTimer(window_, findTimer);
    if (!findBar_.visible() || !document_) return;
    size_t origin = std::min(anchor_, caret_);
    if (navigate) origin = backwards ? std::min(anchor_, caret_) : std::max(anchor_, caret_);
    search_.query(utf8(findBar_.query()), findBar_.matchCase());
    search_.start(origin, backwards);
    revealFind_ = reveal;
    findBar_.status(search_.ready() ? L"" : L"Searching…");
    if (!search_.ready()) SetTimer(window_, findTimer, navigate ? 1 : 60, nullptr);
    InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::searchSlice() {
    if (!findBar_.visible() || !document_ || search_.ready()) { KillTimer(window_, findTimer); return; }
    // Search the live document on the UI thread in short, cancellable slices.
    // An edit or tab change replaces this state before another slice can run.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    const std::function<bool(TextMatch)> accept = diff_ ? std::function<bool(TextMatch)>([this](TextMatch match) { return diff_->inlineRowForText(match.begin, match.end).has_value(); }) : std::function<bool(TextMatch)>{};
    do {
        if (search_.step(document_->text, 8192, accept)) {
            KillTimer(window_, findTimer);
            findBar_.status(!search_.count() ? L"No results" : revealFind_ ? std::to_wstring(search_.ordinal()) + L" / " + std::to_wstring(search_.count()) : std::to_wstring(search_.count()) + L" matches");
            if (revealFind_) revealMatch();
            else InvalidateRect(window_, nullptr, FALSE);
            return;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    SetTimer(window_, findTimer, 1, nullptr);
}
void Viewer::revealMatch() {
    if (!search_.current()) { InvalidateRect(window_, nullptr, FALSE); return; }
    revealRange(*search_.current());
}
bool Viewer::selectSearch(size_t begin, size_t end, const std::string& query, bool matchCase) {
    if (!document_ || diff_ || begin >= end || end > document_->text.size()) return false;
    LiteralPattern pattern; pattern.assign(query, matchCase); TextSearchCursor cursor;
    const auto text = std::string_view(document_->text).substr(begin, end - begin);
    const auto match = pattern.next(text, cursor, text.size());
    if (!match || match->begin != 0 || match->end != text.size()) return false;
    if (html_.active()) setHtmlPreview(false);
    if (csv_.active()) setCsvTable(false);
    revealFind_ = false; revealRange({begin, end}); return true;
}
void Viewer::revealRange(TextMatch match) {
    stopSelection(); clearColumns();
    anchor_ = match.begin; caret_ = match.end; caretTrailing_ = false;
    const size_t patchRow = static_cast<size_t>(std::upper_bound(document_->lines.begin(), document_->lines.end(), match.begin) - document_->lines.begin() - 1);
    auto line = document_->line(patchRow); size_t offset = document_->lines[patchRow], row = patchRow;
    const int origin = textGutter();
    if (diff_) {
        const auto inlineRow = diff_->inlineRowForText(match.begin, match.end); if (!inlineRow) return;
        const auto change = diff_->displayRow(*inlineRow, true);
        after_ = change.after.number != 0;
        const auto source = after_ ? change.after : change.before;
        row = diff_->rowForLine(source, after_, inlineView_); line = diff_->line(source); offset = diff_->offset(source);
    }
    if (wrapping()) row = diff_ ? diffLayout_.rowForOffset(*diff_, row, match.begin - offset, after_) : textLayout_.rowForOffset(*document_, match.begin);
    if (static_cast<int>(row) < top_ + (stickyFile() ? 1 : 0) || static_cast<int>(row) >= top_ + visibleRows())
        top_ = std::max(0, static_cast<int>(row) - std::max(1, visibleRows() / 2));
    if (!wrapping()) {
        int column = 0;
        for (size_t p = 0; p < line.size() && offset + p < match.begin;) { const auto ch = decodeUtf8(line, p); column += ch == '\t' ? 4 - column % 4 : std::max(1, cellWidth(ch)); }
        const int width = contentRect().right / (splitDiff() ? 2 : 1);
        const int columns = std::max(1, (width - origin) / cellWidth_);
        if (column < left_ || column >= left_ + columns) left_ = std::max(0, column - columns / 3);
    }
    scrollbars(); caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
POINT Viewer::caretPoint() const {
    auto point = caretPoint(caret_, caretTrailing_);
    if (!columnRanges_.empty()) {
        const auto& range = columnRanges_[columnActive_];
        point.x += (range.padding + (columnCaretAtEnd_ ? range.virtualWidth : 0)) * cellWidth_;
    }
    return point;
}
POINT Viewer::caretPoint(size_t caret, bool trailing) const {
    if (!document_) return {gutter_, contentTop()};
    const auto& d = *document_;
    if (diff_) {
        const size_t patchRow = static_cast<size_t>(std::upper_bound(document_->lines.begin(), document_->lines.end(), caret) - document_->lines.begin() - 1);
        const DiffLine source{static_cast<uint32_t>(patchRow), 1};
        size_t row = diff_->rowForLine(source, after_, inlineView_);
        const auto line = diff_->line(source); const auto offset = diff_->offset(source);
        if (wrapping()) row = diffLayout_.rowForOffset(*diff_, row, caret - offset, after_, trailing);
        const auto wrapped = wrapping() ? diffLayout_.textRow(*diff_, row, after_) : WrappedRow{};
        int column = wrapping() ? wrapped.column : 0;
        for (size_t p = wrapping() ? wrapped.begin : 0; p < line.size() && offset + p < caret;) {
            const auto c = decodeUtf8(line, p); column += c == '\t' ? 4 - column % 4 : std::max(1, cellWidth(c));
        }
        if (wrapping()) column -= wrapped.column;
        const int origin = textGutter() + (splitDiff() && after_ ? contentRect().right / 2 + 1 : 0);
        return {origin + (column - left_) * cellWidth_, contentTop() + (static_cast<int>(row) - top_) * cellHeight_};
    }
    const size_t row = wrapping() ? textLayout_.rowForOffset(d, caret, trailing) : static_cast<size_t>(std::upper_bound(d.lines.begin(), d.lines.end(), caret) - d.lines.begin() - 1);
    const auto wrapped = wrapping() ? textLayout_.row(d, row) : WrappedRow{};
    const size_t logicalRow = wrapping() ? wrapped.line : row;
    const auto line = d.line(logicalRow); int col = wrapping() ? wrapped.column : 0;
    for (size_t p = wrapping() ? wrapped.begin : 0; p < line.size() && d.lines[logicalRow] + p < caret;) { const auto c = decodeUtf8(line, p); col += c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c)); }
    if (wrapping()) col -= wrapped.column;
    return {gutter_ + (col - left_) * cellWidth_, contentTop() + (static_cast<int>(row) - top_) * cellHeight_};
}
RECT Viewer::inputRect() const {
    const auto rect = contentRect();
    return {textGutter() + (splitDiff() ? rect.right / 2 + 1 : 0), contentTop() + (stickyFile() ? cellHeight_ : 0), rect.right, rect.bottom};
}
void Viewer::caretPosition(bool reveal) {
    if (csv_.active()) return;
    size_t anchor = 0, caret = 0;
    if (!editSelection(anchor, caret)) {
        KillTimer(window_, caretTimer); caretVisible_ = false;
        if (GetFocus() == window_) DestroyCaret();
        return;
    }
    auto point = caretPoint();
    if (reveal) {
        const auto r = contentRect();
        const int top = contentTop() + (stickyFile() ? cellHeight_ : 0);
        const int origin = textGutter() + (splitDiff() ? r.right / 2 + 1 : 0);
        if (point.y < top) top_ -= (top - point.y + cellHeight_ - 1) / cellHeight_;
        else if (point.y + cellHeight_ > r.bottom) top_ += (point.y + cellHeight_ - r.bottom + cellHeight_ - 1) / cellHeight_;
        if (!wrapping()) {
            if (point.x < origin) left_ -= (origin - point.x + cellWidth_ - 1) / cellWidth_;
            else if (point.x + cellWidth_ > r.right) left_ += (point.x + cellWidth_ - r.right + cellWidth_ - 1) / cellWidth_;
        }
        scrollbars(); point = caretPoint();
    }
    if (GetFocus() == window_) {
        positionIme(window_, renderer_.compositionCaret(ime_.text(), point, cellWidth_, cellHeight_, inputRect()), cellHeight_, renderer_.compositionFont());
        caretVisible_ = true; KillTimer(window_, caretTimer);
        const UINT blink = GetCaretBlinkTime();
        if (blink != INFINITE) SetTimer(window_, caretTimer, blink ? blink : 500, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
}
bool Viewer::editSelection(size_t& anchor, size_t& caret) const {
    if (!edit_ || html_.active() || csv_.active()) return false;
    if (!diff_) { anchor = anchor_; caret = caret_; return true; }
    if (!after_) return false;
    const auto first = diff_->workingOffset(anchor_), last = diff_->workingOffset(caret_);
    if (!first || !last) return false;
    anchor = *first; caret = *last; return true;
}
void Viewer::edited() {
    if (diff_) {
        auto comparison = std::make_shared<DiffDocument>(workingDiff(*diffBefore_, edit_->document));
        document_ = std::shared_ptr<const Document>(comparison, &comparison->patch); diff_ = std::move(comparison);
        diffChangeRanges_ = changedDiffRows(*diff_, inlineView_);
        anchor_ = diff_->patchOffset(anchor_); caret_ = diff_->patchOffset(caret_); after_ = true;
        for (auto& range : columnRanges_) { range.begin = diff_->patchOffset(range.begin); range.end = diff_->patchOffset(range.end); }
    }
    updateGitLines();
    layoutDirty_ = true; caretTrailing_ = false; requestFind(false, false, false); scrollbars(); caretPosition(true); InvalidateRect(window_, nullptr, FALSE); if (changed) changed(); if (statusChanged) statusChanged();
}
void Viewer::undo(bool redo) {
    stopSelection(); clearColumns();
    size_t anchor = 0, caret = 0;
    if (!editSelection(anchor, caret)) return;
    if (redo ? edit_->redo(anchor, caret) : edit_->undo(anchor, caret)) { anchor_ = anchor; caret_ = caret; edited(); }
}
void Viewer::selectAll() {
    if (!document_) return;
    stopSelection(); clearColumns();
    anchor_ = 0; caret_ = document_->text.size();
    if (diff_ && edit_) { anchor_ = diff_->patchOffset(0); caret_ = diff_->patchOffset(edit_->document.text.size()); after_ = true; }
    caretPosition(); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::replaceColumnSelection(std::string_view text, bool paste, ColumnErase erase) {
    size_t anchor = 0, caret = 0;
    if (!editSelection(anchor, caret)) return;
    stopSelection();
    auto ranges = columnRanges_;
    if (diff_) for (auto& range : ranges) {
        const auto begin = diff_->workingOffset(range.begin), end = diff_->workingOffset(range.end);
        if (!begin || !end) return;
        range.begin = *begin; range.end = *end;
    }
    if (!replaceColumns(*edit_, ranges, text, paste, erase)) { MessageBeep(MB_ICONWARNING); return; }
    columnRanges_ = std::move(ranges); columnCaretAtEnd_ = true;
    syncColumns(); edited();
}
void Viewer::replace(std::string text, bool paste) {
    size_t anchor = 0, caret = 0;
    if (!editSelection(anchor, caret)) return;
    std::string normalized; const auto newline = edit_->newline();
    for (size_t p = 0; p < text.size(); ++p) {
        if (text[p] == '\r' || text[p] == '\n') { if (text[p] == '\r' && p + 1 < text.size() && text[p + 1] == '\n') ++p; normalized += newline; }
        else normalized += text[p];
    }
    if (!columnRanges_.empty()) { replaceColumnSelection(normalized, paste); return; }
    const size_t begin = std::min(anchor, caret), end = std::max(anchor, caret), length = normalized.size();
    if (edit_->replace(begin, end, std::move(normalized))) { anchor_ = caret_ = begin + length; edited(); }
    else MessageBeep(MB_ICONWARNING);
}
void Viewer::moveCaret(WPARAM key, bool shift, bool ctrl) {
    stopSelection(); clearColumns();
    revealFind_ = false;
    size_t anchor = 0, caret = 0;
    if (!editSelection(anchor, caret)) return;
    const auto& d = edit_->document;
    const size_t row = static_cast<size_t>(std::upper_bound(d.lines.begin(), d.lines.end(), caret) - d.lines.begin() - 1);
    size_t visualRow = wrapping() && !diff_ ? textLayout_.rowForOffset(d, caret, caretTrailing_) : row;
    if (diff_) {
        const size_t patchRow = static_cast<size_t>(std::upper_bound(document_->lines.begin(), document_->lines.end(), caret_) - document_->lines.begin() - 1);
        visualRow = diff_->rowForLine(DiffLine{static_cast<uint32_t>(patchRow), 1}, true, inlineView_);
        if (wrapping()) visualRow = diffLayout_.rowForOffset(*diff_, visualRow, caret - d.lines[row], true, caretTrailing_);
    }
    const auto wrapped = !wrapping() ? WrappedRow{} : diff_ ? diffLayout_.textRow(*diff_, visualRow, true) : textLayout_.row(d, visualRow);
    const auto point = caretPoint();
    caretTrailing_ = false;
    size_t next = caret;
    if (key == VK_LEFT) next = !shift && anchor != caret ? std::min(anchor, caret) : ctrl ? edit_->previousWord(caret) : edit_->previous(caret);
    else if (key == VK_RIGHT) next = !shift && anchor != caret ? std::max(anchor, caret) : ctrl ? edit_->nextWord(caret) : edit_->next(caret);
    else if (key == VK_HOME) next = ctrl ? 0 : d.lines[row] + (wrapping() ? wrapped.begin : 0);
    else if (key == VK_END) {
        next = ctrl ? d.text.size() : d.lines[row] + (wrapping() ? wrapped.end : d.line(row).size());
        caretTrailing_ = wrapping() && !ctrl && wrapped.end < d.line(row).size();
    }
    else {
        const int delta = key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -visibleRows() : visibleRows();
        const auto target = static_cast<size_t>(std::clamp<int>(static_cast<int>(visualRow) + delta, 0, static_cast<int>(rowCount()) - 1));
        if (diff_) {
            for (int at = static_cast<int>(target); at >= 0 && at < static_cast<int>(rowCount()); at += delta < 0 ? -1 : 1) {
                const auto change = diff_->displayRow(diffRow(static_cast<size_t>(at)), inlineView_);
                if (!change.after.number) continue;
                const auto offset = position(point.x, contentTop() + (at - top_) * cellHeight_, &caretTrailing_);
                if (const auto file = diff_->workingOffset(offset)) { next = *file; break; }
            }
        } else next = position(point.x, contentTop() + (static_cast<int>(target) - top_) * cellHeight_, &caretTrailing_);
    }
    caret_ = diff_ ? diff_->patchOffset(next) : next; if (!shift) anchor_ = caret_; caretPosition(true); InvalidateRect(window_, nullptr, FALSE);
}
void Viewer::paintGitMarks() {
    if (!document_ || html_.active()) return;
    const auto r = Scrollbars::contentRect(window_);
    const int left = contentRect().right;
    renderer_.fill({left, 0, r.right, r.bottom}, background);
    auto track = Scrollbars::trackRect(window_, SB_VERT);
    if (!(GetWindowLongPtrW(window_, GWL_STYLE) & WS_VSCROLL)) track = {left, contentTop(), r.right, r.bottom};
    const size_t rows = static_cast<size_t>(std::max(scrollRange_.totalRows(), visibleRows()));
    const auto thumb = Scrollbars::thumbRect(window_, SB_VERT);
    // Exclude the row covered by a sticky filename; include a partial bottom
    // row because its change background is still visible in the editor.
    const size_t first = static_cast<size_t>(top_ + (stickyFile() ? 1 : 0));
    const size_t last = static_cast<size_t>(top_ + std::max(0, (static_cast<int>(r.bottom) - contentTop() + cellHeight_ - 1) / cellHeight_));
    ChangeMarkVisibility visibility{first, last, thumb.top, thumb.bottom};
    const ChangeMarkViewport viewport{first, last, static_cast<int>(thumb.bottom - thumb.top)};
    const int minimum = std::max(1, MulDiv(3, dpi_, 96));
    const auto paintMark = [&](size_t begin, size_t end, bool removed) {
        visibility.include(begin, end);
        const auto mark = changeMark(begin, end, rows, track.top, track.bottom, minimum, viewport);
        renderer_.fill(static_cast<float>(left), static_cast<float>(mark.top), static_cast<float>(r.right), static_cast<float>(mark.bottom),
            removed ? rgb(231,112,118) : rgb(151,195,121));
    };
    if (diff_) {
        for (const auto& change : diffChangeRanges_) {
            const size_t begin = wrapping() ? diffLayout_.rowStart(change.begin) : change.begin;
            const size_t end = wrapping() ? diffLayout_.rowStart(change.end) : change.end;
            paintMark(begin, end, change.removed);
        }
    } else for (const auto& change : gitChangeRanges_) {
        const size_t begin = wrapping() ? textLayout_.lineStart(change.begin) : change.begin;
        const size_t end = wrapping() ? textLayout_.lineStart(change.end) : change.end;
        paintMark(begin, end, change.begin == change.end);
    }
    const auto mask = visibility.mask();
    renderer_.fill(static_cast<float>(left), static_cast<float>(mask.top), static_cast<float>(r.right), static_cast<float>(mask.bottom), background);
}
void Viewer::paint() {
    if (csv_.active()) { csv_.paint(); return; }
    Renderer::Paint paint(renderer_, window_); if (!paint) return;
    auto& dc = renderer_; dc.font(Renderer::Font::Ui);
    const auto r = contentRect();
    if (html_.active()) {
        dc.fill(r, background);
        dc.label({32, 32, std::max<LONG>(32, r.right - 32), r.bottom}, html_.status(), muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else if (image_) {
        dc.fill(r, background);
        const int margin = std::max(1, header_ / 3);
        const RECT bounds{margin, margin, r.right - margin, r.bottom - margin};
        if (!dc.drawImage(bounds, std::max(1, header_ / 4)))
            dc.label(bounds, L"Image cannot be rendered on this device.", muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (!document_) {
        dc.fill(r, background);
        RECT text{32, 48, std::max<LONG>(32, r.right - 32), r.bottom};
        if (!loadingDue_) dc.label(text, message_, muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        const int body = contentTop(), middle = static_cast<int>(r.right) / 2;
        dc.font(Renderer::Font::Code);
        const int rows = std::max(0, (static_cast<int>(r.bottom) - body + cellHeight_ - 1) / cellHeight_);
        const size_t selectionStart = std::min(anchor_, caret_), selectionEnd = std::max(anchor_, caret_);
        std::vector<Token> tokens;
        for (int side = 0; side < (splitDiff() ? 2 : 1); ++side) {
            const int paneLeft = side ? middle + 1 : 0, paneRight = splitDiff() && !side ? middle : static_cast<int>(r.right);
            const int textLeft = paneLeft + textGutter();
            const int columns = std::max(0, (paneRight - textLeft) / cellWidth_ + 1);
            std::vector<Cell> cells(static_cast<size_t>(columns));
            for (int screenRow = 0; screenRow < rows; ++screenRow) {
                const int y = body + screenRow * cellHeight_; const size_t row = static_cast<size_t>(top_ + screenRow);
                auto wrapped = wrapping() && !diff_ && row < rowCount() ? textLayout_.row(*document_, row) : WrappedRow{};
                const size_t logicalRow = diff_ ? diffRow(row) : wrapping() ? wrapped.line : row;
                bool firstRow = !wrapping() || wrapped.begin == 0;
                Color rowBackground = background; DiffLine source; DiffRow change;
                if (diff_ && row < rowCount()) {
                    change = diff_->displayRow(logicalRow, inlineView_);
                    const bool after = inlineView_ ? change.after.number != 0 : side != 0;
                    source = after ? change.after : change.before;
                    if (wrapping()) {
                        wrapped = diffLayout_.textRow(*diff_, row, after);
                        firstRow = diffLayout_.row(row).continuation == 0;
                    }
                    if (source.number && change.changed) rowBackground = after ? addedBackground : removedBackground;
                    else if (!source.number) rowBackground = panel;
                } else if (!diff_ && row < rowCount() && logicalRow < gitChangedLines_.size() && gitChangedLines_[logicalRow]) rowBackground = addedBackground;
                dc.fill({paneLeft, y, paneRight, y + cellHeight_}, rowBackground);
                if (change.heading) continue;
                if (row >= rowCount() || (diff_ && !source.number)) continue;
                if (diff_ && inlineView_) {
                    if (firstRow && change.before.number) dc.label({0, y, std::min(paneRight, gutter_ - 14), y + cellHeight_}, std::to_wstring(change.before.number), muted, DT_RIGHT | DT_SINGLELINE);
                    if (firstRow && change.after.number) dc.label({gutter_, y, std::min(paneRight, textLeft - 14), y + cellHeight_}, std::to_wstring(change.after.number), muted, DT_RIGHT | DT_SINGLELINE);
                } else if (firstRow && !textOnly_) {
                    RECT number{paneLeft, y, std::min(paneRight, textLeft - 14), y + cellHeight_};
                    dc.label(number, std::to_wstring(diff_ ? source.number : logicalRow + 1), muted, DT_RIGHT | DT_SINGLELINE);
                }
                auto line = diff_ ? diff_->line(source) : document_->line(logicalRow);
                const size_t lineOffset = diff_ ? diff_->offset(source) : document_->lines[logicalRow];
                if (!textOnly_) line = line.substr(0, std::min(line.size(), maxLineColumns * 4));
                tokens.clear();
                LexState state = diff_ ? source.state : document_->states[logicalRow];
                lexLine(line, diff_ ? source.language : document_->language, state, &tokens);
                std::fill(cells.begin(), cells.end(), Cell{U' ', foreground, rowBackground, false});
                const int firstColumn = wrapping() ? wrapped.column : left_;
                int col = wrapping() ? wrapped.column : 0; size_t token = 0;
                const size_t end = wrapping() ? wrapped.end : line.size();
                const bool selectedSide = diff_ && edit_ && after_ ? (inlineView_ ? change.after.number != 0 : side != 0) : !splitDiff() || static_cast<bool>(side) == after_;
                TextSearchCursor highlightCursor;
                std::optional<TextMatch> highlight;
                if (findBar_.visible()) highlight = search_.pattern().next(line, highlightCursor, line.size());
                const int columnLimit = textOnly_ ? std::numeric_limits<int>::max() : static_cast<int>(maxLineColumns);
                for (size_t p = wrapping() ? wrapped.begin : 0; p < end && col < firstColumn + columns && col < columnLimit;) {
                    const size_t begin = p; char32_t c = decodeUtf8(line, p);
                    const int width = c == '\t' ? 4 - col % 4 : std::max(1, cellWidth(c));
                    while (token < tokens.size() && tokens[token].end <= begin) ++token;
                    const auto style = token < tokens.size() && tokens[token].begin <= begin ? tokens[token].style : Style::Text;
                    const size_t offset = lineOffset + begin;
                    while (highlight && highlight->end <= begin) highlight = search_.pattern().next(line, highlightCursor, line.size());
                    const bool matched = highlight && begin >= highlight->begin && begin < highlight->end;
                    bool selected = offset >= selectionStart && offset < selectionEnd;
                    if (!columnRanges_.empty()) {
                        const auto next = std::upper_bound(columnRanges_.begin(), columnRanges_.end(), offset,
                            [](size_t p, const ColumnRange& range) { return p < range.begin; });
                        selected = next != columnRanges_.begin() && offset < std::prev(next)->end;
                    }
                    const Color bg = selectedSide && selected ? rgb(50, 70, 94) : matched ? rgb(92, 73, 34) : rowBackground;
                    if (c < 32) c = U' ';
                    for (int i = 0; i < width; ++i) {
                        const int x = col + i - firstColumn; if (x < 0 || x >= columns) continue;
                        const bool wideContinuation = cellWidth(c) == 2 && i == 1;
                        cells[x] = {i == 0 ? c : U' ', styles[static_cast<size_t>(style)], bg, wideContinuation};
                        if (i > 0 && col < firstColumn) cells[x] = {U' ', foreground, bg, false};
                    }
                    col += width;
                }
                RECT clip{textLeft, body, paneRight, r.bottom}; dc.cells(textLeft, y, cellWidth_, cellHeight_, cells, columns, clip);
                if (selectedSide && !columnRanges_.empty()) {
                    const size_t firstByte = lineOffset + (wrapping() ? wrapped.begin : 0), lastByte = lineOffset + end;
                    auto range = std::lower_bound(columnRanges_.begin(), columnRanges_.end(), firstByte,
                        [](const ColumnRange& range, size_t p) { return range.end < p; });
                    const auto fill = [&](RECT bounds, Color color) { RECT clipped{}; if (IntersectRect(&clipped, &bounds, &clip)) dc.fill(clipped, color); };
                    const auto rangePoint = [&](size_t offset, bool trailing) -> std::optional<POINT> {
                        if (offset < firstByte || offset > lastByte) return {};
                        if (wrapping() && ((offset == lastByte && !trailing && end < line.size()) ||
                            (offset == firstByte && trailing && wrapped.begin))) return {};
                        int column = wrapping() ? wrapped.column : 0;
                        for (size_t p = wrapping() ? wrapped.begin : 0; p < offset - lineOffset;) {
                            const auto c = decodeUtf8(line, p); column += c == '\t' ? 4 - column % 4 : std::max(1, cellWidth(c));
                        }
                        return POINT{textLeft + (column - firstColumn) * cellWidth_, y};
                    };
                    for (; range != columnRanges_.end() && range->begin <= lastByte; ++range) {
                        const auto endPoint = rangePoint(range->end, range->trailing);
                        if (range->virtualWidth && endPoint) {
                            const int x = endPoint->x + range->padding * cellWidth_;
                            fill({x, y, x + range->virtualWidth * cellWidth_, y + cellHeight_}, rgb(50, 70, 94));
                        }
                        if (edit_ && caretVisible_ && GetFocus() == window_ && ime_.text().text.empty()) {
                            if (auto point = rangePoint(columnCaretAtEnd_ ? range->end : range->begin, columnCaretAtEnd_ && range->trailing)) {
                                point->x += (range->padding + (columnCaretAtEnd_ ? range->virtualWidth : 0)) * cellWidth_;
                                fill({point->x, y, point->x + 2, y + cellHeight_}, foreground);
                            }
                        }
                    }
                }
            }
        }
        if (splitDiff()) dc.fill({middle, body, middle + 1, r.bottom}, border);
        if (diff_) {
            dc.font(Renderer::Font::Ui);
            for (int screenRow = 0; screenRow < rows; ++screenRow) {
                const size_t row = static_cast<size_t>(top_ + screenRow);
                if (row >= rowCount()) break;
                const auto change = diff_->displayRow(diffRow(row), inlineView_);
                if (!change.heading) continue;
                const int y = body + screenRow * cellHeight_;
                dc.fill({0, y, r.right, y + cellHeight_}, panel);
                const auto file = diff_->fileForRow(diffRow(row), inlineView_);
                const auto text = file && diff_->rows[file->row].heading == change.heading ?
                    std::string_view(file->path) : document_->line(change.heading - 1);
                dc.label({14, y, r.right - 12, y + cellHeight_}, wide(text), accent);
            }
            if (const auto file = body < r.bottom ? stickyFile() : nullptr) {
                // Paint after both panes so the filename stays fixed across
                // wrapping and horizontal scrolling, above all source cells.
                const int bottom = std::min(body + cellHeight_, static_cast<int>(r.bottom));
                dc.fill({0, body, r.right, bottom}, panel);
                dc.label({14, body, r.right - 12, bottom}, wide(file->path), accent);
                dc.fill({0, bottom - 1, r.right, bottom}, border);
            }
        }
        if (edit_ && columnRanges_.empty() && caretVisible_ && GetFocus() == window_ && ime_.text().text.empty()) {
            const auto point = caretPoint();
            const RECT caret{point.x, point.y, point.x + 2, point.y + cellHeight_};
            const RECT bodyRect{textGutter() + (splitDiff() ? middle + 1 : 0), body + (stickyFile() ? cellHeight_ : 0), r.right, r.bottom};
            RECT clipped{};
            if (IntersectRect(&clipped, &caret, &bodyRect)) dc.fill(clipped, foreground);
        }
        if (edit_ && GetFocus() == window_) dc.composition(ime_.text(), caretPoint(), cellWidth_, cellHeight_, inputRect());
    }
    paintGitMarks();
    if (!html_.active()) scrollbars_.paint(window_, dc, background);
}
LRESULT CALLBACK Viewer::procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<Viewer*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) { self = static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = w; SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT Viewer::handle(UINT m, WPARAM a, LPARAM b) {
    if (html_.active() && (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL || m == WM_VSCROLL || m == WM_HSCROLL)) return 0;
    LRESULT result = 0;
    if (scrollbars_.handle(window_, m, a, b, result)) return result;
    if (csv_.active() && m == WM_KEYDOWN && findKey(MSG{window_, m, a, b, 0, {0, 0}})) return 0;
    if (csv_.active() && csv_.handle(m, a, b, result)) return result;
    switch (m) {
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: stopSelection(); clearColumns(); scrollbars(); findLayout(); htmlLayout(); caretPosition(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_MOVE: htmlLayout(); return 0;
    case WM_SETTINGCHANGE: caretPosition(); return 0;
    case WM_SETFOCUS: caretPosition(); if (focused) focused(); if (html_.active()) html_.focus(); return 0;
    case WM_KILLFOCUS:
        stopSelection();
        ime_.clear();
        highSurrogate_ = 0; caretVisible_ = false; KillTimer(window_, caretTimer); DestroyCaret();
        InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_TIMER:
        if (a == loadingTimer) {
            // A queued timer from the previous tab must not shorten this wait.
            if (loadingDue_ && GetTickCount64() >= loadingDue_) {
                KillTimer(window_, loadingTimer); loadingDue_ = 0;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        if (a == selectionTimer) { scrollSelection(); return 0; }
        if (a == findTimer) { searchSlice(); return 0; }
        if (a == caretTimer && edit_ && GetFocus() == window_ && GetCaretBlinkTime() != INFINITE) {
            caretVisible_ = !caretVisible_; InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY: setFileDropTarget(nullptr); html_.close(); stopLoading(); stopSelection(); ime_.clear(); KillTimer(window_, caretTimer); KillTimer(window_, findTimer); return 0;
    case WM_SHOWWINDOW: if (!a) { stopSelection(); ime_.cancel(window_); } break;
    case WM_SETCURSOR:
        if (image_ && LOWORD(b) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        return DefWindowProcW(window_, m, a, b);
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        SetFocus(window_);
        const POINT point{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        if (!document_) return 0;
        if (html_.active() || csv_.active()) return 0;
        if (point.y < contentTop() + (stickyFile() ? cellHeight_ : 0)) return 0;
        if (point.x >= contentRect().right || point.y >= contentRect().bottom || !rowCount()) return 0;
        stopSelection(); clearColumns(); ime_.cancel(window_);
        revealFind_ = false;
        bool after = splitDiff() && point.x >= contentRect().right / 2;
        if (diff_ && inlineView_ && rowCount()) {
            const size_t row = std::min(rowCount() - 1, static_cast<size_t>((point.y - contentTop()) / cellHeight_ + top_));
            after = diff_->displayRow(diffRow(row), true).after.number != 0;
        }
        const bool extend = (GetKeyState(VK_SHIFT) & 0x8000) && (!splitDiff() || after == after_);
        after_ = after;
        if (m == WM_MBUTTONDOWN || m == WM_MBUTTONDBLCLK) {
            const int origin = textGutter() + (splitDiff() && after_ ? contentRect().right / 2 + 1 : 0);
            columnStart_ = std::clamp((static_cast<int>(point.x) - origin + cellWidth_ / 2) / cellWidth_ + left_, 0, static_cast<int>(maxLineColumns));
            columnStartRow_ = std::min(rowCount() - 1, static_cast<size_t>((point.y - contentTop()) / cellHeight_ + top_));
            columnDragging_ = true;
        }
        if (m == WM_LBUTTONDBLCLK) {
            stopSelection();
            WordRange word{anchor_, caret_};
            position(point.x, point.y, nullptr, &word);
            anchor_ = word.begin; caret_ = word.end; caretTrailing_ = false;
            caretPosition(); InvalidateRect(window_, nullptr, FALSE); return 0;
        }
        if (!extend && !columnDragging_) anchor_ = position(point.x, point.y);
        selecting_ = true; SetCapture(window_); SetTimer(window_, selectionTimer, 50, nullptr);
        extendSelection(point.x, point.y); return 0;
    }
    case WM_MOUSEMOVE:
        if (selecting_) {
            if (!(a & (columnDragging_ ? MK_MBUTTON : MK_LBUTTON))) stopSelection();
            else extendSelection(GET_X_LPARAM(b), GET_Y_LPARAM(b));
        }
        return 0;
    case WM_MBUTTONUP: case WM_LBUTTONUP:
        if (selecting_ && columnDragging_ == (m == WM_MBUTTONUP)) { extendSelection(GET_X_LPARAM(b), GET_Y_LPARAM(b)); stopSelection(); }
        return 0;
    case WM_CANCELMODE: case WM_CAPTURECHANGED: stopSelection(); return 0;
    case WM_MOUSEWHEEL: scroll(-GET_WHEEL_DELTA_WPARAM(a) / WHEEL_DELTA * 3, 0); return 0;
    case WM_VSCROLL: case WM_HSCROLL: {
        const bool vertical = m == WM_VSCROLL; int& pos = vertical ? top_ : left_;
        SCROLLINFO info{sizeof(info), SIF_ALL, 0, 0, 0, 0, 0}; GetScrollInfo(window_, vertical ? SB_VERT : SB_HORZ, &info);
        switch (LOWORD(a)) {
        case SB_LINEUP: --pos; break; case SB_LINEDOWN: ++pos; break;
        case SB_PAGEUP: pos -= static_cast<int>(info.nPage); break; case SB_PAGEDOWN: pos += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = scrollbars_.trackPosition(vertical ? SB_VERT : SB_HORZ, info.nTrackPos); break;
        case SB_TOP: pos = 0; break; case SB_BOTTOM: pos = info.nMax; break;
        default: return 0;
        }
        scroll(0, 0); return 0;
    }
    case WM_SYSKEYDOWN: case WM_KEYDOWN:
        if (html_.active()) return DefWindowProcW(window_, m, a, b);
        if (ime_.active()) return DefWindowProcW(window_, m, a, b);
        if (edit_ && !(GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) &&
            (GetKeyState(VK_MENU) & 0x8000) && (a == VK_UP || a == VK_DOWN)) {
            revealFind_ = false;
            stopSelection(); clearColumns();
            size_t anchor = 0, caret = 0;
            if (editSelection(anchor, caret) && edit_->moveLines(anchor, caret, a == VK_DOWN)) { anchor_ = anchor; caret_ = caret; edited(); }
            return 0;
        }
        if (m == WM_SYSKEYDOWN) break;
        if (a == VK_ESCAPE && (selecting_ || !columnRanges_.empty())) {
            stopSelection(); clearColumns(); caretPosition(); InvalidateRect(window_, nullptr, FALSE); return 0;
        }
        if (findKey(MSG{window_, m, a, b, 0, {0, 0}})) return 0;
        revealFind_ = false;
        if (edit_) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && a == 'S') { if (save) save(); return 0; }
            if (ctrl && a == 'X') { cut(); return 0; }
            if ((ctrl && a == 'V') || (shift && a == VK_INSERT)) { replace(utf8(clipboardText(window_)), true); return 0; }
            if (ctrl && (a == 'Z' || a == 'Y')) { undo(a == 'Y' || shift); return 0; }
            if (a == VK_DELETE || a == VK_BACK) {
                if (!columnRanges_.empty()) {
                    replaceColumnSelection("", false, a == VK_DELETE ? ColumnErase::Forward : ctrl ? ColumnErase::WordBackward : ColumnErase::Backward);
                    return 0;
                }
                size_t anchor = 0, caret = 0;
                if (!editSelection(anchor, caret)) return 0;
                if (anchor == caret) {
                    if (a == VK_BACK) anchor = ctrl ? edit_->previousWord(caret) : edit_->previous(caret); else caret = edit_->next(caret);
                    anchor_ = diff_ ? diff_->patchOffset(anchor) : anchor; caret_ = diff_ ? diff_->patchOffset(caret) : caret;
                }
                replace(""); return 0;
            }
            if (a == VK_TAB) { replace("\t"); return 0; }
            if (a == VK_LEFT || a == VK_RIGHT || a == VK_UP || a == VK_DOWN || a == VK_HOME || a == VK_END || a == VK_PRIOR || a == VK_NEXT) { moveCaret(a, shift, ctrl); return 0; }
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && a == 'C') { copy(); return 0; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && a == 'A') { selectAll(); return 0; }
        if (a == VK_UP) scroll(-1, 0); else if (a == VK_DOWN) scroll(1, 0);
        else if (a == VK_LEFT) scroll(0, -4); else if (a == VK_RIGHT) scroll(0, 4);
        else if (a == VK_PRIOR) scroll(-visibleRows(), 0); else if (a == VK_NEXT) scroll(visibleRows(), 0);
        else if (a == VK_HOME) { top_ = left_ = 0; scroll(0, 0); }
        else if (a == VK_END && document_) { top_ = static_cast<int>(rowCount()); scroll(0, 0); }
        return 0;
    case WM_CHAR:
        if (html_.active()) return 0;
        if (ime_.active()) return 0;
        if (edit_ && a != 0x7f && !(GetKeyState(VK_CONTROL) & 0x8000) && (a >= 32 || a == '\r')) {
            const wchar_t c = static_cast<wchar_t>(a);
            if (c >= 0xd800 && c <= 0xdbff) { highSurrogate_ = c; return 0; }
            std::wstring text; if (highSurrogate_) { text += highSurrogate_; highSurrogate_ = 0; } text += c;
            replace(utf8(text));
        }
        return 0;
    case WM_IME_SETCONTEXT: case WM_INPUTLANGCHANGE: case WM_IME_NOTIFY:
    case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION: case WM_IME_COMPOSITION:
        if (!edit_ && (m == WM_IME_STARTCOMPOSITION || m == WM_IME_COMPOSITION)) return 0;
        return ime_.handle(window_, m, a, b, [this] { caretPosition(); },
            [this](std::wstring text) { if (edit_) replace(utf8(text)); });
    case WM_CONTEXTMENU: {
        if (image_ || html_.active()) return 0;
        revealFind_ = false;
        HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING | (hasSelection() ? 0 : MF_GRAYED), 1, L"Copy\tCtrl+C"); AppendMenuW(menu, MF_STRING, 2, L"Select all\tCtrl+A");
        if (edit_) {
            size_t anchor = 0, caret = 0;
            const auto flags = MF_STRING | (editSelection(anchor, caret) ? 0 : MF_GRAYED);
            AppendMenuW(menu, flags, 3, L"Cut\tCtrl+X"); AppendMenuW(menu, flags, 4, L"Paste\tCtrl+V"); AppendMenuW(menu, flags, 5, L"Undo\tCtrl+Z"); AppendMenuW(menu, flags, 6, L"Redo\tCtrl+Y"); AppendMenuW(menu, MF_STRING, 7, L"Save\tCtrl+S");
        }
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b)}; if (p.x == -1) { p = {gutter_, header_}; ClientToScreen(window_, &p); }
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window_, nullptr); DestroyMenu(menu);
        if (command == 1) copy(); else if (command == 2) selectAll();
        else if (edit_) { if (command == 3) cut(); else if (command == 4) replace(utf8(clipboardText(window_)), true); else if (command == 5 || command == 6) undo(command == 6); else if (command == 7 && save) save(); }
        return 0;
    }
    default: return DefWindowProcW(window_, m, a, b);
    }
    return DefWindowProcW(window_, m, a, b);
}
}

size_t poppy::Viewer::documentBytes() const {
    size_t bytes = textLayout_.bytes() + diffLayout_.bytes() + (document_ ? document_->text.capacity() + document_->lines.capacity() * sizeof(uint32_t) + document_->states.capacity() * sizeof(LexState) : 0);
    bytes += csv_.bytes();
    bytes += columnRanges_.capacity() * sizeof(ColumnRange);
    bytes += gitChangedLines_.capacity() / 8;
    bytes += gitChangeRanges_.capacity() * sizeof(LineChange);
    bytes += diffChangeRanges_.capacity() * sizeof(DiffChange);
    if (gitBefore_) bytes += gitBefore_->text.capacity() + gitBefore_->lines.capacity() * sizeof(uint32_t) + gitBefore_->states.capacity() * sizeof(LexState);
    if (diff_) {
        bytes += diff_->rows.capacity() * sizeof(DiffRow) + diff_->inlineOrder.capacity() * sizeof(uint32_t) + diff_->files.capacity() * sizeof(DiffFile);
        bytes += diff_->workingLines.capacity() * sizeof(DiffDocument::WorkingLine);
        if (edit_) bytes += edit_->document.text.capacity() + edit_->baseline.capacity();
        if (diffBefore_) bytes += diffBefore_->text.capacity() + diffBefore_->lines.capacity() * sizeof(uint32_t) + diffBefore_->states.capacity() * sizeof(LexState);
        for (const auto& file : diff_->files) bytes += file.path.capacity();
    }
    if (image_) bytes += image_->pixels.capacity() * 2; // CPU pixels and the device bitmap.
    return bytes;
}
