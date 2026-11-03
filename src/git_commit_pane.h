#pragma once
#include "tree_input.h"
#include "resource.h"
#include <map>

namespace poppy {
// Persistent source-control input. Drafts belong to individual repositories.
class GitCommitPane {
public:
    struct Repository { ResourceId root; std::wstring label; };
    void create(HWND parent, HINSTANCE instance);
    HWND window() const { return window_; }
    void setDpi(UINT dpi);
    void repositories(std::vector<Repository> repositories);
    const ResourceId& selectedRepository() const { return root_; }
    void busy(bool value);
    void committed(const ResourceId& root, const std::wstring& message);
    bool contains(HWND window) const { return window == window_ || IsChild(window_, window); }
    bool key(const MSG& message);
    std::function<void(const ResourceId&, const std::wstring&, bool)> requested;
    std::function<void()> repositorySelected;
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void layout();
    void activate(int part);
    void submit(bool amend);
    void select(size_t index);
    RECT button(int part) const;
    int hit(LPARAM point) const;
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    bool enabled() const { return !busy_ && !repositories_.empty(); }
    HWND window_ = nullptr, input_ = nullptr;
    TreeInput message_;
    Renderer renderer_;
    UINT dpi_ = 96;
    std::vector<Repository> repositories_;
    std::map<std::string, std::wstring> drafts_;
    ResourceId root_;
    bool busy_ = false;
    int hover_ = -1, pressed_ = -1, focus_ = 1;
};
}
