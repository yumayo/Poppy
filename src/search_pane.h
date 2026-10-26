#pragma once
#include "find_bar.h"
#include "project_provider.h"
#include "edit_buffer.h"
#include "scrollbars.h"

namespace poppy {
struct SearchOverlay { std::string relative; std::shared_ptr<EditBuffer> buffer; };
class SearchPane {
public:
    ~SearchPane() { stop(); }
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void setContext(const ResourceId& root, std::shared_ptr<ProjectProvider> provider);
    void active(bool visible);
    void focus() { input_.show(); }
    void refresh();
    void cancel();
    void stop();
    HWND window() const { return window_; }
    std::function<std::vector<SearchOverlay>()> overlays;
    std::function<void(const ContentMatch&, const std::string&, bool)> opened;
    std::function<void()> exit;
private:
    struct Job;
    static bool deliver(const std::shared_ptr<Job>& job, std::vector<ContentMatch> batch);
    static void notify(const std::shared_ptr<Job>& job);
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void start();
    void drain();
    void scanOverlay();
    void paint();
    void layout();
    void move(int delta);
    void accept();
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    int page() const;
    int body() const { return px(132); }
    void status(std::wstring text);
    HWND window_ = nullptr;
    Renderer renderer_;
    Scrollbars scrollbars_;
    FindBar input_;
    UINT dpi_ = 96;
    ResourceId root_;
    std::shared_ptr<ProjectProvider> provider_;
    std::unique_ptr<Worker> worker_;
    std::shared_ptr<Job> job_;
    std::vector<ContentMatch> matches_;
    std::vector<SearchOverlay> overlays_;
    std::unique_ptr<ContentScanner> overlayScanner_;
    size_t overlayIndex_ = 0, overlayOffset_ = 0, selected_ = 0;
    uint64_t overlayRevision_ = 0;
    int top_ = 0;
    bool active_ = false, respectIgnore_ = true, overlayDone_ = true;
    ContentSearchOptions options_;
    ContentSearchSummary overlaySummary_;
    std::wstring status_ = L"Type to search this project.";
};
}
