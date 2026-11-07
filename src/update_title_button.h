#pragma once
#include "render.h"

namespace poppy {
inline constexpr UINT restartUpdateCommand = 0x1F10;
class UpdateTitleButton {
public:
    void create(HWND owner, HINSTANCE instance);
    void available(bool value);
    bool available() const { return available_; }
    void layout();
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void paint();
    void cancel();
    bool contains(LPARAM point) const;
    HWND window_ = nullptr, owner_ = nullptr;
    Renderer renderer_;
    UINT dpi_ = 0;
    bool available_ = false, hover_ = false, pressed_ = false;
};
}
