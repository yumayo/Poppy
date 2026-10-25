#pragma once
#include "render.h"
#include "session.h"
#include "hook_server.h"
#include <memory>

namespace poppy {
// A retained project surface owns its models, controls and independent workers.
// Hiding the surface never tears down sessions or rebuilds the tree.
class ProjectRuntime {
public:
    ProjectRuntime();
    ~ProjectRuntime();
    void create(HWND parent, HINSTANCE instance, HookServer* hooks);
    void initialize(const fs::path& path, const ProjectSession& state, bool saved);
    void activate(bool active);
    void shutdown();
    bool canClose();
    void revalidate();
    void setDpi(UINT dpi);
    void poll();
    bool key(const MSG& message);
    ProjectSession snapshot();
    AiSummary summary() const;
    std::wstring description() const;
    bool apply(const AiEvent& event);
    void focusAttention();
    size_t documentBytes() const;
    void releaseDocument();
    HWND window() const;
    bool ready() const;
    std::function<void(const fs::path&)> openFolder;
    std::function<void()> changed;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
