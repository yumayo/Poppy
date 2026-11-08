#pragma once
#include "terminal.h"
#include "tree_input.h"
#include "ai_status_win.h"
#include "session.h"
#include "project_provider.h"
#include <memory>
#include <optional>
#include <set>

namespace poppy {
class ShutdownDialog;
// Each session retains its ConPTY, VT model and view while other tabs are shown.
struct TerminalSession {
    ~TerminalSession() { if (terminal) { terminal->stop(); terminal->starting = {}; terminal->ended = {}; terminal->stopping = {}; terminal->shutdownFailed = {}; } }
    SavedTerminal configuration;
    std::unique_ptr<Terminal> terminal;
    std::string notificationId;
    std::wstring notificationError;
    std::set<std::string> reportedErrors;
    bool hostStatusReceived = false;
    AiStatus ai;
    std::wstring wslTool;
};
class TerminalPane {
public:
    ~TerminalPane();
    void create(HWND parent, HINSTANCE instance, bool docked = false);
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
    void pollNotifications();
    void focusAttention();
    bool hasSession(const std::string& id) const;
    void activateSession(const std::string& id);
    const std::string& activeSessionId() const { return active().configuration.id; }
    AiSummary summary() const;
    std::wstring description() const;
    void stop(ShutdownDialog* progress = nullptr);
    void detach(ShutdownDialog* progress = nullptr);
    bool key(const MSG& message);
    bool contains(HWND window) const;
    size_t dockCount() const { return sessions_.size(); }
    const SavedTerminal& dockConfiguration(size_t index) const { return sessions_[index]->configuration; }
    std::wstring dockTitle(size_t index) const { return sessionName(*sessions_[index]); }
    const AiStatus& dockStatus(size_t index) const { return sessions_[index]->ai; }
    HWND dockWindow(size_t index) const { return sessions_[index]->terminal->window(); }
    void dockSelect(size_t index, int group);
    void dockPlace(size_t index, int group, bool displayed) { sessions_[index]->configuration.group = group; sessions_[index]->configuration.displayed = displayed; }
    void dockClose(size_t index) { close(index); }
    void dockMenu(size_t index, POINT point, RECT bounds);
    void dockAdd(int group, bool dropdown, POINT point);
    std::function<void()> dockChanged;
    HWND window() const { return active().terminal->window(); }
    HWND container() const { return window_; }
    std::function<void()> changed;
    std::function<void(const std::string&)> activated;
    std::function<void(const std::string&, const std::wstring&, const std::wstring&)> error;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    void finishRename(bool commit);
    LRESULT handle(UINT, WPARAM, LPARAM);
    TerminalSession& active() { return *sessions_[active_]; }
    const TerminalSession& active() const { return *sessions_[active_]; }
    void add(SavedTerminal configuration = {}, bool reconnect = false);
    void select(size_t index);
    void close(size_t index);
    void menu(size_t index, POINT point);
    void addAi(const std::wstring& tool, int group);
    void addShell(int group, TerminalShell shell);
    void addMenu(int group, POINT point);
    void layout();
    void paint();
    void reportError(TerminalSession& session, const std::string& code, const std::wstring& message);
    std::wstring sessionName(const TerminalSession& session) const;
    std::vector<RECT> bounds() const;
    void normalizeGroups();
    void drag(POINT point);
    void endDrag(bool drop = false);
    size_t groupTab(int group) const;
    RECT groupBounds(int group) const;
    RECT addButtonBounds(int group, bool dropdown) const;
    int tabsRight(int group) const { return addButtonBounds(group, false).left; }
    HWND dockParent_ = nullptr;
    RECT dockRename_{};
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
    std::wstring tooltipText_;
};
}
