#pragma once
#include "win.h"

namespace poppy {
// Shows a count in the top-right corner of the window icon, which the taskbar
// button, the title bar and Alt+Tab display. Zero restores the class icons.
class TaskbarBadge {
public:
    TaskbarBadge() = default;
    ~TaskbarBadge();
    TaskbarBadge(const TaskbarBadge&) = delete;
    TaskbarBadge& operator=(const TaskbarBadge&) = delete;
    void attach(HWND window, HINSTANCE instance, int iconId);
    // Cheap when nothing changed, so it can run on every timer tick.
    void update(size_t count, UINT dpi);
private:
    void apply();
    void release();
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    int iconId_ = 0;
    HICON big_ = nullptr, small_ = nullptr;
    size_t count_ = 0, shown_ = 0;
    UINT dpi_ = 96, shownDpi_ = 0;
};
}
