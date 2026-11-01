#pragma once
#include "terminal.h"
#include "tree_input.h"
#include "ai_status_win.h"
#include "hook_pipe.h"
#include "session.h"
#include "project_provider.h"
#include <memory>
#include <optional>

namespace poppy {
class ShutdownDialog;
// Each session retains its ConPTY, VT model and view while other tabs are shown.
struct TerminalSession {
    ~TerminalSession() { if (terminal) { terminal->stop(); terminal->starting = {}; terminal->ended = {}; terminal->stopping = {}; } }
    SavedTerminal configuration;
    std::unique_ptr<Terminal> terminal;
    HookPipe events;
    std::string notificationId;
    std::wstring notificationError;
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
    void connect(AiEventQueue* hooks) { events_ = hooks; }
    bool apply(const AiEvent& event);
    void acknowledge();
    bool animateAiStatus();
    void focusAttention();
    AiSummary summary() const;
    std::wstring description() const;
    void stop(ShutdownDialog* progress = nullptr);
    bool key(const MSG& message);
    bool contains(HWND window) const { return window == window_ || IsChild(window_, window); }
    HWND window() const { return active().terminal->window(); }
    HWND container() const { return window_; }
    std::function<void()> changed;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    void finishRename(bool commit);
    LRESULT handle(UINT, WPARAM, LPARAM);
    TerminalSession& active() { return *sessions_[active_]; }
    const TerminalSession& active() const { return *sessions_[active_]; }
    void add(SavedTerminal configuration = {});
    void select(size_t index);
    void close(size_t index);
    void menu(size_t index, POINT point);
    void addAi(const std::wstring& tool, int group);
    void addShell(int group, TerminalShell shell);
    void addMenu(int group, POINT point);
    void layout();
    void paint();
    std::wstring sessionName(const TerminalSession& session) const;
    std::vector<RECT> bounds() const;
    void normalizeGroups();
    void drag(POINT point);
    void endDrag(bool drop = false);
    size_t groupTab(int group) const;
    RECT groupBounds(int group) const;
    RECT addButtonBounds(int group, bool dropdown) const;
    int tabsRight(int group) const { return addButtonBounds(group, false).left; }
    HWND window_ = nullptr, rename_ = nullptr, tooltips_ = nullptr;
    size_t renaming_ = 0, tools_ = 0;
    HINSTANCE instance_ = nullptr;
    Renderer renderer_{D2D1_PRESENT_OPTIONS_IMMEDIATELY};
    TreeInput renameInput_;
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
    TerminalShell defaultShell_ = TerminalShell::PowerShell;
    AiEventQueue* events_ = nullptr;
    ShutdownDialog* shutdownProgress_ = nullptr;
    std::wstring tooltipText_;
};
}
