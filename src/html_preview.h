#pragma once
#include "resource.h"
#include <string_view>

namespace poppy {
bool isHtmlExtension(std::wstring_view extension);
// An escaped file URI preserves relative CSS, scripts and images.
std::string htmlFileUri(const ResourceId& path);
}

#ifdef _WIN32
#include "win.h"
namespace poppy {
class HtmlPreview {
public:
    ~HtmlPreview();
    void show(HWND parent, const ResourceId& path, const RECT& bounds);
    // Unload the page while keeping WebView2 ready for the next preview.
    void hide();
    // Release WebView2 before the parent window is destroyed.
    void close();
    void layout(const RECT& bounds);
    void focus();
    bool active() const;
    std::wstring status() const;
    std::function<void()> changed, focused;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
#endif
