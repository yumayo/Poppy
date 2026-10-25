#pragma once
#include "render.h"
#include "file_search.h"
#include "project_provider.h"
#include <memory>

namespace poppy {
class QuickOpen {
public:
    ~QuickOpen() { stop(); }
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void setProvider(std::shared_ptr<ProjectProvider> provider) { provider_ = std::move(provider); }
    void setRoot(const ResourceId& root);
    void show();
    void close(bool restoreFocus = true);
    void layout();
    void refresh();
    void stop();
    bool visible() const { return visible_; }
    HWND window() const { return window_; }
    std::function<void(const ResourceId&)> opened;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void request();
    void changed();
    void replace(std::wstring text);
    void accept();
    void move(int delta);
    void imePosition();
    void caretChanged();
    size_t previous(size_t position) const;
    size_t next(size_t position) const;
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    int rows() const;
    HWND window_ = nullptr, previousFocus_ = nullptr;
    Renderer renderer_;
    std::unique_ptr<Worker> worker_, indexWorker_;
    std::atomic<uint64_t> revision_{0}, indexRevision_{0};
    UINT dpi_ = 96;
    ResourceId root_;
    std::shared_ptr<ProjectProvider> provider_;
    std::shared_ptr<const FileIndex> index_;
    std::wstring query_;
    FileSearchResult result_;
    size_t caret_ = 0, anchor_ = 0, selected_ = 0, first_ = 0, inputFirst_ = 0;
    int caretX_ = 0, wheel_ = 0;
    wchar_t highSurrogate_ = 0;
    bool visible_ = false, loading_ = false, composing_ = false, indexing_ = false, indexAgain_ = false;
};
}
