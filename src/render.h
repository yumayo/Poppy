#pragma once
#include "win.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

namespace poppy {
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
    enum class Font { Ui, Code };
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
    void font(Font value) { font_ = value; }
    bool begin(HWND window);
    // Offscreen rendering uses WIC, including the pixel regression tests.
    bool begin(IWICBitmap* bitmap);
    HRESULT end();
    void discard();
    void fill(const RECT& rect, COLORREF color);
    int textWidth(const std::wstring& text) const;
    void label(const RECT& rect, const std::wstring& text, COLORREF color,
               UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    void cells(int x, int y, int cellWidth, int cellHeight,
               const std::vector<Cell>& cells, int columns, const RECT& clip);
    void line(float x1, float y1, float x2, float y2, COLORREF color, float width);
    void ellipse(const RECT& rect, COLORREF color, float width);
    void outline(const RECT& rect, COLORREF color, float width = 1.0f);
private:
    bool prepare(HRESULT result);
    void color(COLORREF value);
    HWND window_ = nullptr;
    ComPtr<ID2D1HwndRenderTarget> windowTarget_;
    ComPtr<ID2D1RenderTarget> bitmapTarget_;
    IWICBitmap* bitmap_ = nullptr; // Retained by bitmapTarget_.
    ID2D1RenderTarget* target_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> uiFont_, codeFont_;
    Font font_ = Font::Ui;
    int cellWidth_ = 8, textHeight_ = 18;
};
}
