#pragma once
#include "terminal.h"
#include "hook_server.h"
#include "session.h"
#include "project_provider.h"
#include <memory>
#include <optional>

namespace poppy {
// Each session retains its ConPTY, VT model and view while other tabs are shown.
struct TerminalSession {
    ~TerminalSession() { if (terminal) { terminal->stop(); terminal->starting = {}; terminal->ended = {}; terminal->stopping = {}; } }
    SavedTerminal configuration;
    std::unique_ptr<Terminal> terminal;
    std::string notificationId;
    AiStatus ai;
    std::string wslToken;
    std::wstring wslTool;
};
class TerminalPane {
public:
    ~TerminalPane();
    void create(HWND parent, HINSTANCE instance);
    void setDpi(UINT dpi);
    void setDirectory(const ResourceId& directory);
    void relocate(const ResourceId& source, const ResourceId& destination);
    void setProvider(std::shared_ptr<ProjectProvider> provider) { provider_ = std::move(provider); }
    void restore(const ProjectSession& state);
    void snapshot(ProjectSession& state) const;
    void connect(HookServer* hooks) { hooks_ = hooks; }
    bool apply(const AiEvent& event);
    void acknowledge();
    void focusAttention();
    AiSummary summary() const;
    std::wstring description() const;
    void stop();
    bool key(const MSG& message);
    bool contains(HWND window) const { return window == window_ || IsChild(window_, window); }
    HWND window() const { return active().terminal->window(); }
    HWND container() const { return window_; }
    std::function<void()> changed;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK renameProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT handle(UINT, WPARAM, LPARAM);
    TerminalSession& active() { return *sessions_[active_]; }
    const TerminalSession& active() const { return *sessions_[active_]; }
    void add(SavedTerminal configuration = {});
    void select(size_t index);
    void close(size_t index);
    void menu(size_t index, POINT point);
    void addAi(const std::wstring& tool);
    void addMenu(POINT point);
    void layout();
    void paint();
    std::vector<RECT> bounds() const;
    void normalizeGroups();
    void drag(POINT point);
    void endDrag(bool drop = false);
    size_t groupTab(int group) const;
    RECT groupBounds(int group) const;
    HWND window_ = nullptr, rename_ = nullptr, tooltips_ = nullptr;
    size_t renaming_ = 0, tools_ = 0;
    HINSTANCE instance_ = nullptr;
    Renderer renderer_;
    UINT dpi_ = 96;
    int header_ = 32, offset_[2]{}, ratio_ = 500;
    RECT splitter_{};
    bool resizing_ = false;
    struct Drag { size_t index; POINT origin, point; bool active = false; int group = -1; size_t position = 0; };
    std::optional<Drag> drag_;
    DropPreview preview_;
    ResourceId directory_;
    std::shared_ptr<ProjectProvider> provider_;
    std::vector<std::unique_ptr<TerminalSession>> sessions_;
    size_t active_ = 0;
    HookServer* hooks_ = nullptr;
    std::wstring tooltipText_;
};
}
