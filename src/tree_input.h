#pragma once
#include "render.h"

namespace poppy {
// A borderless editor drawn with the tree font and a configurable background.
class TreeInput {
public:
    HWND create(HWND parent, HINSTANCE instance, const std::wstring& text, COLORREF color, COLORREF backgroundColor = RGB(50, 70, 94));
    void setDpi(UINT dpi);
    const std::wstring& text() const { return text_; }
    void draw(Renderer& renderer, const RECT& rect) const;
    std::function<void(bool)> finished;
    bool commitOnFocusLoss = false;
private:
    struct State { std::wstring text; size_t caret, anchor; };
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void replace(std::wstring text);
    void undo(bool redo);
    void caretChanged();
    size_t previous(size_t position) const;
    size_t next(size_t position) const;
    size_t word(size_t position, bool forward) const;
    size_t hit(int x) const;
    HWND window_ = nullptr;
    Renderer renderer_;
    mutable InputLine line_;
    ImeComposition ime_;
    UINT dpi_ = 96;
    COLORREF color_ = foreground;
    COLORREF background_ = RGB(50, 70, 94);
    std::wstring text_, cue_;
    std::vector<State> undo_, redo_;
    size_t caret_ = 0, anchor_ = 0, limit_ = 255;
    wchar_t highSurrogate_ = 0;
    bool caretVisible_ = true;
};
}
