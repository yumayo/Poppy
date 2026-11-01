#pragma once
#include "render.h"
#include <optional>

namespace poppy {
// Used only after closing has been accepted. Waits keep this modal surface
// responsive without dispatching application work or terminal input again.
class ShutdownDialog {
public:
    enum class Stage { Saving, Stopping, Finishing };
    ShutdownDialog(HWND owner, HINSTANCE instance);
    ~ShutdownDialog();
    ShutdownDialog(const ShutdownDialog&) = delete;
    ShutdownDialog& operator=(const ShutdownDialog&) = delete;
    void stage(Stage stage, std::wstring context = {});
    void detail(std::wstring text);
    // The task must contain only blocking I/O or waits, never UI operations.
    void wait(const std::function<void()>& task);
    HWND window() const { return window_; }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK shadeProcedure(HWND, UINT, WPARAM, LPARAM);
    void layout();
    void paint();
    HWND owner_ = nullptr, shade_ = nullptr, window_ = nullptr;
    Renderer renderer_, shadeRenderer_;
    UINT dpi_ = 96;
    bool ownerEnabled_ = false;
    Stage stage_ = Stage::Saving;
    std::wstring context_, detail_;
    ULONGLONG started_ = 0;
    std::optional<int> quit_;
    int px(int value) const { return MulDiv(value, dpi_, 96); }
};
}
