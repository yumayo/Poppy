#pragma once
#include "render.h"
#include <shobjidl.h>

namespace poppy {
// Shows a count on the window's taskbar button as an overlay icon.
// Call reset() for every "TaskbarButtonCreated" message: the button, and with
// it the overlay, is recreated when Explorer restarts.
class TaskbarBadge {
public:
    TaskbarBadge() = default;
    ~TaskbarBadge();
    TaskbarBadge(const TaskbarBadge&) = delete;
    TaskbarBadge& operator=(const TaskbarBadge&) = delete;
    void reset(HWND window);
    // Cheap when nothing changed, so it can run on every timer tick.
    void update(size_t count, UINT dpi);
private:
    void apply();
    HWND window_ = nullptr;
    ComPtr<ITaskbarList3> taskbar_;
    HICON icon_ = nullptr;
    size_t count_ = 0, shown_ = 0;
    UINT dpi_ = 96, shownDpi_ = 0;
};
}
