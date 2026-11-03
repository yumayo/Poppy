#pragma once
#include "win.h"
#include "image_preview.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

namespace poppy {
class InputLine;
// Small COM owner, also supported by the MinGW build.
template<class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** put() { reset(); return &value_; }
    void reset() { if (value_) { value_->Release(); value_ = nullptr; } }
private:
    T* value_ = nullptr;
};

// Initialize on the UI thread before creating windows; failures reach wWinMain.
void initializeRendering();

class Renderer {
public:
    explicit Renderer(D2D1_PRESENT_OPTIONS presentOptions = D2D1_PRESENT_OPTIONS_NONE) : presentOptions_(presentOptions) {}
    enum class Font { Ui, Code };
    enum class Icon { Explorer, SourceControl, Branch, OpenAllChanges, Search, StageChanges, UnstageChanges, DiscardChanges, StashChanges, OpenFile, Eye, EyeClosed, CollapseAll, Spinner, OpenRevision };
    // Keeps both the Win32 paint validation and the Direct2D drawing balanced,
    // including empty windows and failed render-target creation.
    class Paint {
    public:
        Paint(Renderer& renderer, HWND window);
        ~Paint();
        Paint(const Paint&) = delete;
        Paint& operator=(const Paint&) = delete;
        explicit operator bool() const { return drawing_; }
    private:
        Renderer& renderer_;
        HWND window_;
        PAINTSTRUCT paint_{};
        bool drawing_;
    };

    void setDpi(UINT dpi);
    int cellWidth() const { return cellWidth_; }
    int textHeight() const { return textHeight_; }
    const LOGFONTW& compositionFont(Font font = Font::Code) const { return font == Font::Ui ? uiCompositionFont_ : compositionFont_; }
    void font(Font value) { font_ = value; }
    bool begin(HWND window);
    // Offscreen rendering uses WIC, including the pixel regression tests.
    bool begin(IWICBitmap* bitmap);
    HRESULT end();
    void discard();
    void fill(const RECT& rect, COLORREF color, float opacity = 1.0f);
    // Preserve fractional positions for smoothly moving overview markers.
    void fill(float left, float top, float right, float bottom, COLORREF color);
    void fillRounded(const RECT& rect, COLORREF color, float radius);
    int textWidth(const std::wstring& text, bool italic = false) const;
    void label(const RECT& rect, const std::wstring& text, COLORREF color,
               UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, bool italic = false, int lineHeight = 0);
    void cells(int x, int y, int cellWidth, int cellHeight,
               const std::vector<Cell>& cells, int columns, const RECT& clip);
    // Preedit uses the exact same cell renderer as committed terminal text.
    void composition(const CompositionText& text, POINT origin, int cellWidth, int cellHeight, const RECT& clip);
    void composition(const CompositionLayout& layout, POINT origin, int cellWidth, int cellHeight, const RECT& clip, COLORREF preeditBackground = background);
    POINT compositionCaret(const CompositionText& text, POINT origin, int cellWidth, int cellHeight, const RECT& clip) const;
    void line(float x1, float y1, float x2, float y2, COLORREF color, float width);
    void ellipse(const RECT& rect, COLORREF color, float width);
    void outline(const RECT& rect, COLORREF color, float width = 1.0f);
    void icon(Icon icon, const RECT& rect, COLORREF color, float rotation = 0.0f);
    void setImage(std::shared_ptr<const ImagePreview> image);
    bool drawImage(const RECT& bounds, int checkerSize);
private:
    friend class InputLine;
    bool prepare(HRESULT result);
    void color(COLORREF value, float opacity = 1.0f);
    HWND window_ = nullptr;
    ComPtr<ID2D1HwndRenderTarget> windowTarget_;
    ComPtr<ID2D1RenderTarget> bitmapTarget_;
    IWICBitmap* bitmap_ = nullptr; // Retained by bitmapTarget_.
    ID2D1RenderTarget* target_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::shared_ptr<const ImagePreview> image_;
    ComPtr<ID2D1Bitmap> imageBitmap_;
    ComPtr<IDWriteTextFormat> uiFont_, codeFont_;
    D2D1_PRESENT_OPTIONS presentOptions_;
    Font font_ = Font::Ui;
    int cellWidth_ = 8, textHeight_ = 18;
    LOGFONTW compositionFont_{}, uiCompositionFont_{};
};

// Single-line UI input: shape committed text and preedit together. The same
// DirectWrite layout supplies drawing, selection, hit testing and IME geometry.
class InputLine {
public:
    void update(const Renderer& renderer, const std::wstring& text, size_t caret, size_t anchor,
                const CompositionText& composition, const RECT& bounds);
    void draw(Renderer& renderer, COLORREF color, COLORREF selection, bool showCaret) const;
    POINT caret() const { return caret_; }
    int height() const { return height_; }
    size_t hit(int x) const;
    void reset() { scroll_ = 0; }
private:
    void range(Renderer& renderer, size_t begin, size_t length, COLORREF color, int underline = 0) const;
    ComPtr<IDWriteTextLayout> layout_;
    CompositionText composition_;
    RECT bounds_{};
    POINT caret_{};
    float scroll_ = 0;
    size_t selectionBegin_ = 0, selectionEnd_ = 0, compositionBegin_ = 0;
    int height_ = 1, caretWidth_ = 1;
};

// A child overlay keeps the drop target visible above the editor/ConPTY views.
class DropPreview {
public:
    void create(HWND parent, HINSTANCE instance);
    void show(const RECT& rect);
    void hide() { ShowWindow(window_, SW_HIDE); }
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    HWND window_ = nullptr;
    Renderer renderer_;
};
}
