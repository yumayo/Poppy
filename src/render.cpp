#include "render.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <imm.h>

namespace poppy {
namespace {
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ").");
}
struct Factories {
    ComPtr<ID2D1Factory> drawing;
    ComPtr<IDWriteFactory> text;
    ComPtr<IDWriteTypography> typography;
    ComPtr<ID2D1StrokeStyle> iconStroke;
    std::array<ComPtr<ID2D1PathGeometry>, 10> icons;
    Factories() {
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, drawing.put()), "Direct2D initialization failed");
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(text.put())), "DirectWrite initialization failed");
        check(text->CreateTypography(typography.put()), "DirectWrite typography creation failed");
        // Code and terminal cells keep their own advances, including 'fi'/'fl'.
        check(typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES, 0}), "DirectWrite typography setup failed");
        check(typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES, 0}), "DirectWrite typography setup failed");
        auto stroke = D2D1::StrokeStyleProperties();
        stroke.startCap = stroke.endCap = D2D1_CAP_STYLE_ROUND;
        stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
        check(drawing->CreateStrokeStyle(stroke, nullptr, 0, iconStroke.put()), "Icon stroke creation failed");
        for (size_t i = 0; i < icons.size(); ++i) {
            check(drawing->CreatePathGeometry(icons[i].put()), "Icon geometry creation failed");
            ComPtr<ID2D1GeometrySink> sink;
            check(icons[i]->Open(sink.put()), "Icon geometry setup failed");
            const auto path = [&](std::initializer_list<D2D1_POINT_2F> points, bool closed = false) {
                auto point = points.begin(); sink->BeginFigure(*point++, D2D1_FIGURE_BEGIN_HOLLOW);
                for (; point != points.end(); ++point) sink->AddLine(*point);
                sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            };
            const auto circle = [&](float x, float y) {
                constexpr float radius = 2.75f;
                sink->BeginFigure({x - radius, y}, D2D1_FIGURE_BEGIN_HOLLOW);
                for (const float side : {1.0f, -1.0f})
                    sink->AddArc(D2D1::ArcSegment({x + side * radius, y}, {radius, radius}, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            };
            switch (static_cast<Renderer::Icon>(i)) {
            case Renderer::Icon::Explorer:
                path({{6, 18}, {3, 18}, {3, 3}, {16, 3}, {16, 5}});
                path({{8, 8}, {16, 8}, {21, 13}, {21, 22}, {8, 22}}, true);
                path({{16, 8}, {16, 13}, {21, 13}});
                break;
            case Renderer::Icon::SourceControl:
                path({{6, 7}, {6, 17}}); path({{18, 7}, {18, 9}, {6, 15}});
                circle(6, 4); circle(18, 4); circle(6, 20);
                break;
            case Renderer::Icon::Branch:
                path({{5, 7}, {5, 17}}); path({{5, 12}, {11, 12}, {18, 7}});
                circle(5, 4); circle(18, 4); circle(5, 20);
                break;
            case Renderer::Icon::OpenAllChanges:
                path({{7, 4}, {7, 2}, {20, 2}, {20, 18}, {18, 18}});
                path({{3, 6}, {12, 6}, {16, 10}, {16, 22}, {3, 22}}, true);
                path({{12, 6}, {12, 10}, {16, 10}});
                path({{6, 12}, {10, 12}});
                path({{8, 15}, {8, 19}}); path({{6, 17}, {10, 17}});
                break;
            case Renderer::Icon::Search:
                sink->BeginFigure({3, 10}, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment({17, 10}, {7, 7}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->AddArc(D2D1::ArcSegment({3, 10}, {7, 7}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                path({{15, 15}, {21, 21}});
                break;
            case Renderer::Icon::StageChanges:
                path({{12, 5}, {12, 19}});
                path({{5, 12}, {19, 12}});
                break;
            case Renderer::Icon::UnstageChanges:
                path({{5, 12}, {19, 12}});
                break;
            case Renderer::Icon::DiscardChanges:
                path({{8, 4}, {3, 9}, {8, 14}});
                path({{3, 9}, {15, 9}, {20, 13}, {20, 17}, {16, 21}, {9, 21}});
                break;
            case Renderer::Icon::StashChanges:
                path({{3, 13}, {3, 21}, {21, 21}, {21, 13}, {16, 13}, {16, 16}, {8, 16}, {8, 13}}, true);
                path({{12, 3}, {12, 12}});
                path({{8, 8}, {12, 12}, {16, 8}});
                break;
            case Renderer::Icon::OpenFile:
                path({{10, 3}, {4, 3}, {4, 21}, {20, 21}, {20, 14}});
                path({{14, 3}, {21, 3}, {21, 10}});
                path({{21, 3}, {11, 13}});
                break;
            }
            check(sink->Close(), "Icon geometry completion failed");
        }
    }
};
Factories& factories() { static Factories value; return value; }
D2D1_RECT_F rectangle(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom));
}
bool empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

struct CompositionGeometry {
    CompositionLayout layout;
    int top;
};
CompositionGeometry compositionGeometry(const CompositionText& text, POINT origin, int cw, int ch, const RECT& clip) {
    auto layout = text.layout(static_cast<int>(clip.right - clip.left) / cw, static_cast<int>(origin.x - clip.left) / cw);
    // Keep the preedit cursor and candidate anchor visible when wrapping on the
    // last line. Earlier composition rows are clipped to the text viewport.
    const int top = std::min(static_cast<int>(origin.y), static_cast<int>(clip.bottom) - (layout.caretRow + 1) * ch);
    return {std::move(layout), top};
}

// These renderers are used synchronously by IDWriteTextLayout::Draw, never retained.
class GlyphRunRenderer : public IDWriteTextRenderer {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(IDWritePixelSnapping)) || IsEqualIID(id, __uuidof(IDWriteTextRenderer))) {
            *result = static_cast<IDWriteTextRenderer*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override { *matrix = {1, 0, 0, 1, 0, 0}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override { *pixels = 1.0f; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
};

// DirectWrite supplies shaping and fallback fonts. Place every glyph cluster at
// its model cell, rather than letting fallback-font advances move later cells.
class CellTextRenderer final : public GlyphRunRenderer {
public:
    CellTextRenderer(ID2D1RenderTarget* target, ID2D1Brush* brush, float x, const std::vector<float>& positions)
        : target_(target), brush_(brush), x_(x), positions_(positions) {}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override {
        D2D1_MATRIX_3X2_F transform{}; target_->GetTransform(&transform);
        *matrix = {transform._11, transform._12, transform._21, transform._22, transform._31, transform._32}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override { *pixels = 1.0f; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT baselineX, FLOAT baselineY, DWRITE_MEASURING_MODE mode,
                                         const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION* description, IUnknown*) override {
        if (!description || !description->clusterMap) {
            target_->DrawGlyphRun(D2D1::Point2F(baselineX, baselineY), run, brush_, mode); return S_OK;
        }
        const UINT32 missing = std::numeric_limits<UINT32>::max();
        std::vector<UINT32> clusters(run->glyphCount, missing);
        for (UINT32 i = 0; i < description->stringLength; ++i) {
            const UINT32 glyph = description->clusterMap[i];
            if (glyph < run->glyphCount) clusters[glyph] = std::min(clusters[glyph], description->textPosition + i);
        }
        std::vector<FLOAT> advances(run->glyphCount, 0.0f);
        std::vector<DWRITE_GLYPH_OFFSET> offsets(run->glyphCount);
        float position = 0.0f, advance = 0.0f;
        for (UINT32 i = 0; i < run->glyphCount; ++i) {
            if (clusters[i] != missing) {
                if (clusters[i] >= positions_.size()) return E_INVALIDARG;
                position = positions_[clusters[i]]; advance = 0.0f;
            }
            if (run->glyphOffsets) offsets[i] = run->glyphOffsets[i];
            offsets[i].advanceOffset += position + advance;
            if (run->glyphAdvances) advance += run->glyphAdvances[i];
        }
        auto fixed = *run;
        fixed.glyphAdvances = advances.data(); fixed.glyphOffsets = offsets.data();
        // Positions above are physical left-to-right model columns, even when
        // DirectWrite resolves an RTL run inside a terminal or source-code row.
        fixed.bidiLevel = 0;
        target_->DrawGlyphRun(D2D1::Point2F(x_, baselineY), &fixed, brush_, mode);
        return S_OK;
    }
private:
    ID2D1RenderTarget* target_;
    ID2D1Brush* brush_;
    float x_;
    const std::vector<float>& positions_;
};
}

void initializeRendering() { (void)factories(); }
Renderer::Paint::Paint(Renderer& renderer, HWND window) : renderer_(renderer), window_(window), drawing_(false) {
    BeginPaint(window_, &paint_); drawing_ = renderer_.begin(window_);
}
Renderer::Paint::~Paint() { if (drawing_) renderer_.end(); EndPaint(window_, &paint_); }

void Renderer::setDpi(UINT dpi) {
    const float scale = static_cast<float>(dpi) / 96.0f;
    auto& text = factories().text;
    check(text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"", uiFont_.put()), "UI font creation failed");
    check(text->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                DWRITE_FONT_STRETCH_NORMAL, 15.0f * scale, L"", codeFont_.put()), "Code font creation failed");
    check(codeFont_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "Code font setup failed");
    ComPtr<IDWriteTextLayout> layout;
    check(text->CreateTextLayout(L"M", 1, codeFont_.get(), 1000.0f, 1000.0f, layout.put()), "Font measurement failed");
    DWRITE_TEXT_METRICS metrics{};
    check(layout->GetMetrics(&metrics), "Font measurement failed");
    DWRITE_LINE_METRICS line{}; UINT32 lines = 0;
    check(layout->GetLineMetrics(&line, 1, &lines), "Font baseline measurement failed");
    cellWidth_ = std::max(1, static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)));
    textHeight_ = std::max(1, static_cast<int>(std::ceil(metrics.height)));
    check(codeFont_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, static_cast<float>(textHeight_), std::ceil(line.baseline)), "Code line spacing setup failed");
    // IMM font metadata uses physical pixels and an average-width hint. Actual
    // preedit glyph placement is controlled by composition(), just like cells().
    compositionFont_ = {};
    check(codeFont_->GetFontFamilyName(compositionFont_.lfFaceName, LF_FACESIZE), "IME font name lookup failed");
    compositionFont_.lfHeight = -static_cast<LONG>(std::lround(codeFont_->GetFontSize()));
    compositionFont_.lfWidth = cellWidth_;
    compositionFont_.lfWeight = static_cast<LONG>(codeFont_->GetFontWeight());
    compositionFont_.lfItalic = codeFont_->GetFontStyle() != DWRITE_FONT_STYLE_NORMAL;
    compositionFont_.lfCharSet = DEFAULT_CHARSET;
    compositionFont_.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    uiCompositionFont_ = {};
    check(uiFont_->GetFontFamilyName(uiCompositionFont_.lfFaceName, LF_FACESIZE), "UI IME font name lookup failed");
    uiCompositionFont_.lfHeight = -static_cast<LONG>(std::lround(uiFont_->GetFontSize()));
    uiCompositionFont_.lfWeight = static_cast<LONG>(uiFont_->GetFontWeight());
    uiCompositionFont_.lfCharSet = DEFAULT_CHARSET;
    uiCompositionFont_.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    // Geometry and fonts use the existing Win32 physical-pixel layout. Keeping
    // targets at 96 DPI avoids scaling it twice on per-monitor DPI changes.
}
void Renderer::discard() {
    imageBitmap_.reset();
    brush_.reset(); target_ = nullptr; windowTarget_.reset(); bitmapTarget_.reset(); bitmap_ = nullptr;
}
void Renderer::setImage(std::shared_ptr<const ImagePreview> image) {
    imageBitmap_.reset(); image_ = std::move(image);
}
bool Renderer::drawImage(const RECT& bounds, int checkerSize) {
    if (!image_) return false;
    if (empty(bounds)) return true;
    if (!imageBitmap_) {
        const auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(target_->CreateBitmap(D2D1::SizeU(image_->width, image_->height), image_->pixels.data(), image_->width * 4, properties, imageBitmap_.put()))) return false;
    }
    const auto placement = fitImage(image_->width, image_->height, bounds.right - bounds.left, bounds.bottom - bounds.top);
    const RECT destination{bounds.left + placement.x, bounds.top + placement.y,
        bounds.left + placement.x + placement.width, bounds.top + placement.y + placement.height};
    fill(destination, RGB(48, 53, 62));
    const int tile = std::max(1, checkerSize);
    for (int y = 0; y < placement.height; y += tile) for (int x = 0; x < placement.width; x += tile)
        if ((x / tile + y / tile) % 2 == 0) fill({destination.left + x, destination.top + y,
            destination.left + std::min(x + tile, placement.width), destination.top + std::min(y + tile, placement.height)}, RGB(68, 74, 84));
    target_->DrawBitmap(imageBitmap_.get(), rectangle(destination), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
}
bool Renderer::prepare(HRESULT result) {
    if (SUCCEEDED(result) && !brush_) result = target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f), brush_.put());
    if (FAILED(result)) { discard(); return false; }
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    target_->BeginDraw(); return true;
}
bool Renderer::begin(HWND window) {
    window_ = window;
    if (bitmapTarget_) discard();
    RECT bounds{}; GetClientRect(window, &bounds);
    if (empty(bounds)) return false;
    const auto size = D2D1::SizeU(static_cast<UINT32>(bounds.right), static_cast<UINT32>(bounds.bottom));
    HRESULT result = S_OK;
    if (!windowTarget_) {
        auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        const auto windowProperties = D2D1::HwndRenderTargetProperties(window, size, presentOptions_);
        result = factories().drawing->CreateHwndRenderTarget(properties, windowProperties, windowTarget_.put());
        if (FAILED(result)) {
            properties.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
            result = factories().drawing->CreateHwndRenderTarget(properties, windowProperties, windowTarget_.put());
        }
    } else {
        const auto previous = windowTarget_->GetPixelSize();
        if (previous.width != size.width || previous.height != size.height) {
            result = windowTarget_->Resize(size);
            if (FAILED(result)) { discard(); return begin(window); }
        }
    }
    target_ = windowTarget_.get(); return prepare(result);
}
bool Renderer::begin(IWICBitmap* bitmap) {
    window_ = nullptr;
    if (!bitmap) return false;
    if (windowTarget_ || (bitmap_ && bitmap_ != bitmap)) discard();
    HRESULT result = S_OK;
    if (!bitmapTarget_) {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        result = factories().drawing->CreateWicBitmapRenderTarget(bitmap, properties, bitmapTarget_.put());
        if (SUCCEEDED(result)) bitmap_ = bitmap;
    }
    target_ = bitmapTarget_.get(); return prepare(result);
}
HRESULT Renderer::end() {
    const HRESULT result = target_->EndDraw();
    if (FAILED(result)) {
        discard();
        if (result == static_cast<HRESULT>(D2DERR_RECREATE_TARGET) && window_) InvalidateRect(window_, nullptr, FALSE);
    }
    return result;
}
void Renderer::color(COLORREF value, float opacity) {
    brush_->SetColor(D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f, opacity));
}
void Renderer::fill(const RECT& rect, COLORREF value, float opacity) {
    if (empty(rect)) return;
    color(value, opacity); target_->FillRectangle(rectangle(rect), brush_.get());
}
void Renderer::fillRounded(const RECT& rect, COLORREF value, float radius) {
    if (empty(rect)) return;
    color(value); target_->FillRoundedRectangle(D2D1::RoundedRect(rectangle(rect), radius, radius), brush_.get());
}
int Renderer::textWidth(const std::wstring& text) const {
    auto* format = font_ == Font::Code ? codeFont_.get() : uiFont_.get();
    if (!format || text.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factories().text->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 1000000.0f, 1000000.0f, layout.put()))) return 0;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(layout->GetMetrics(&metrics)) ? static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)) : 0;
}
void InputLine::update(const Renderer& renderer, const std::wstring& text, size_t caret, size_t anchor,
                       const CompositionText& composition, const RECT& bounds) {
    bounds_ = bounds; composition_ = composition;
    if (!renderer.uiFont_) { layout_.reset(); caret_ = {bounds.left, bounds.top}; return; }
    caret = std::min(caret, text.size()); anchor = std::min(anchor, text.size());
    selectionBegin_ = std::min(caret, anchor); selectionEnd_ = std::max(caret, anchor);
    compositionBegin_ = selectionBegin_;
    std::wstring display = text;
    if (!composition.text.empty()) {
        display.replace(selectionBegin_, selectionEnd_ - selectionBegin_, std::wstring(composition.text.begin(), composition.text.end()));
        caret = compositionBegin_ + std::min(composition.cursor, composition.text.size());
        selectionEnd_ = selectionBegin_;
    }
    // IMM offsets are UTF-16; never place a cursor inside a surrogate pair.
    if (caret && caret < display.size() && display[caret] >= 0xdc00 && display[caret] <= 0xdfff &&
        display[caret - 1] >= 0xd800 && display[caret - 1] <= 0xdbff) --caret;
    const float width = static_cast<float>(std::max(1L, bounds.right - bounds.left));
    check(factories().text->CreateTextLayout(display.data(), static_cast<UINT32>(display.size()), renderer.uiFont_.get(),
        width, static_cast<float>(std::max(1L, bounds.bottom - bounds.top)), layout_.put()), "Input layout failed");
    layout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    FLOAT x = 0, y = 0; DWRITE_HIT_TEST_METRICS hit{};
    layout_->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &x, &y, &hit);
    DWRITE_TEXT_METRICS metrics{}; layout_->GetMetrics(&metrics);
    caretWidth_ = std::max(1, static_cast<int>(std::lround(renderer.uiFont_->GetFontSize() / 13.0f)));
    const float available = std::max(0.0f, width - caretWidth_);
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, metrics.widthIncludingTrailingWhitespace - available));
    if (x < scroll_) scroll_ = x;
    if (x > scroll_ + available) scroll_ = x - available;
    caret_ = {bounds.left + static_cast<LONG>(std::lround(x - scroll_)), bounds.top + static_cast<LONG>(std::lround(y))};
    height_ = std::max(1, static_cast<int>(std::ceil(hit.height)));
}
size_t InputLine::hit(int x) const {
    if (!layout_) return 0;
    BOOL trailing = FALSE, inside = FALSE; DWRITE_HIT_TEST_METRICS hit{};
    layout_->HitTestPoint(static_cast<float>(x - bounds_.left) + scroll_, static_cast<float>(caret_.y - bounds_.top), &trailing, &inside, &hit);
    return hit.textPosition + (trailing ? hit.length : 0);
}
void InputLine::range(Renderer& renderer, size_t begin, size_t length, COLORREF color, int underline) const {
    if (!length) return;
    UINT32 count = 0;
    layout_->HitTestTextRange(static_cast<UINT32>(begin), static_cast<UINT32>(length),
        static_cast<float>(bounds_.left) - scroll_, static_cast<float>(bounds_.top), nullptr, 0, &count);
    std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
    if (!count || FAILED(layout_->HitTestTextRange(static_cast<UINT32>(begin), static_cast<UINT32>(length),
        static_cast<float>(bounds_.left) - scroll_, static_cast<float>(bounds_.top), ranges.data(), count, &count))) return;
    for (const auto& area : ranges) {
        const auto bottom = static_cast<LONG>(std::ceil(area.top + area.height));
        renderer.fill({static_cast<LONG>(std::floor(area.left)), underline ? bottom - underline : static_cast<LONG>(std::floor(area.top)),
            static_cast<LONG>(std::ceil(area.left + area.width)), bottom}, color);
    }
}
void InputLine::draw(Renderer& renderer, COLORREF color, COLORREF selection, bool showCaret) const {
    if (!layout_ || empty(bounds_)) return;
    renderer.target_->PushAxisAlignedClip(rectangle(bounds_), D2D1_ANTIALIAS_MODE_ALIASED);
    range(renderer, selectionBegin_, selectionEnd_ - selectionBegin_, selection);
    const auto attributes = [&](const auto& draw) {
        for (size_t begin = 0; begin < composition_.text.size();) {
            const auto attribute = begin < composition_.attributes.size() ? composition_.attributes[begin] : uint8_t{ATTR_INPUT};
            size_t end = begin + 1;
            while (end < composition_.text.size() && (end < composition_.attributes.size() ? composition_.attributes[end] : uint8_t{ATTR_INPUT}) == attribute) ++end;
            draw(compositionBegin_ + begin, end - begin, attribute == ATTR_TARGET_CONVERTED || attribute == ATTR_TARGET_NOTCONVERTED);
            begin = end;
        }
    };
    attributes([&](size_t begin, size_t length, bool target) { if (target) range(renderer, begin, length, RGB(50, 70, 94)); });
    renderer.color(color);
    renderer.target_->DrawTextLayout(D2D1::Point2F(static_cast<float>(bounds_.left) - scroll_, static_cast<float>(bounds_.top)), layout_.get(), renderer.brush_.get());
    attributes([&](size_t begin, size_t length, bool target) { range(renderer, begin, length, target ? accent : muted, target ? 2 : 1); });
    if (showCaret) renderer.fill({caret_.x, caret_.y, caret_.x + caretWidth_, caret_.y + height_}, composition_.text.empty() ? color : accent);
    renderer.target_->PopAxisAlignedClip();
}
void Renderer::label(const RECT& rect, const std::wstring& text, COLORREF value, UINT flags) {
    auto* format = font_ == Font::Code ? codeFont_.get() : uiFont_.get();
    if (empty(rect) || text.empty() || !format) return;
    ComPtr<IDWriteTextLayout> layout;
    auto& factory = factories().text;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
        static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top), layout.put()))) return;
    layout->SetTextAlignment(flags & DT_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : flags & DT_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(flags & DT_VCENTER ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetWordWrapping(flags & DT_WORDBREAK ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS)) {
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, ellipsis.put()))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, flags & DT_PATH_ELLIPSIS ? static_cast<UINT32>(L'\\') : 0u, flags & DT_PATH_ELLIPSIS ? 1u : 0u};
            layout->SetTrimming(&trimming, ellipsis.get());
        }
    }
    color(value);
    target_->DrawTextLayout(D2D1::Point2F(static_cast<float>(rect.left), static_cast<float>(rect.top)), layout.get(), brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Renderer::cells(int x, int y, int cw, int ch, const std::vector<Cell>& cells, int columns, const RECT& clip) {
    const int count = std::min(columns, static_cast<int>(cells.size()));
    RECT row{x, y, x + count * cw, y + ch}, visible{};
    if (!codeFont_ || count <= 0 || !IntersectRect(&visible, &row, &clip)) return;
    target_->PushAxisAlignedClip(rectangle(visible), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int col = 0; col < count;) {
        const int begin = col; const auto bg = cells[col].bg;
        while (col < count && cells[col].bg == bg) ++col;
        fill({x + begin * cw, y, x + col * cw, y + ch}, bg);
    }
    for (int col = 0; col < count;) {
        if (cells[col].continuation) { ++col; continue; }
        const int begin = col; const auto fg = cells[col].fg;
        std::wstring text; std::vector<float> positions;
        while (col < count && cells[col].fg == fg) {
            if (cells[col].continuation) { ++col; continue; }
            char32_t scalar = cells[col].ch;
            const int width = poppy::cellWidth(scalar) == 2 ? 2 : 1;
            if (scalar < 32 || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) scalar = U' ';
            positions.push_back(static_cast<float>((col - begin) * cw));
            if (scalar > 0xffff) {
                scalar -= 0x10000;
                text += static_cast<wchar_t>(0xd800 + (scalar >> 10)); text += static_cast<wchar_t>(0xdc00 + (scalar & 1023));
                positions.push_back(positions.back());
            } else text += static_cast<wchar_t>(scalar);
            col += width;
        }
        ComPtr<IDWriteTextLayout> layout;
        if (text.find_first_not_of(L' ') == std::wstring::npos || FAILED(factories().text->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), codeFont_.get(),
            static_cast<float>((col - begin) * cw), static_cast<float>(ch), layout.put()))) continue;
        layout->SetTypography(factories().typography.get(), {0, static_cast<UINT32>(text.size())});
        color(fg);
        CellTextRenderer renderer(target_, brush_.get(), static_cast<float>(x + begin * cw), positions);
        layout->Draw(nullptr, &renderer, static_cast<float>(x + begin * cw), static_cast<float>(y));
    }
    target_->PopAxisAlignedClip();
}
POINT Renderer::compositionCaret(const CompositionText& text, POINT origin, int cw, int ch, const RECT& clip) const {
    if (text.text.empty() || cw <= 0 || ch <= 0 || empty(clip)) return origin;
    const auto geometry = compositionGeometry(text, origin, cw, ch, clip);
    return {clip.left + geometry.layout.caretColumn * cw, geometry.top + geometry.layout.caretRow * ch};
}
void Renderer::composition(const CompositionText& text, POINT origin, int cw, int ch, const RECT& clip) {
    if (text.text.empty() || cw <= 0 || ch <= 0 || empty(clip)) return;
    const auto geometry = compositionGeometry(text, origin, cw, ch, clip);
    composition(geometry.layout, {clip.left, geometry.top}, cw, ch, clip);
}
void Renderer::composition(const CompositionLayout& layout, POINT origin, int cw, int ch, const RECT& clip, COLORREF preeditBackground) {
    if (layout.glyphs.empty() || cw <= 0 || ch <= 0 || empty(clip)) return;
    const auto& glyphs = layout.glyphs;
    for (size_t begin = 0; begin < glyphs.size();) {
        size_t end = begin + 1;
        while (end < glyphs.size() && glyphs[end].row == glyphs[begin].row) ++end;
        const auto& first = glyphs[begin]; const auto& last = glyphs[end - 1];
        const int columns = last.column + last.width - first.column;
        std::vector<Cell> row(static_cast<size_t>(columns));
        for (size_t i = begin; i < end; ++i) {
            const auto& glyph = glyphs[i];
            const bool target = glyph.attribute == ATTR_TARGET_CONVERTED || glyph.attribute == ATTR_TARGET_NOTCONVERTED;
            Cell cell; cell.ch = glyph.character; cell.bg = target ? rgb(50, 70, 94) : preeditBackground;
            row[glyph.column - first.column] = cell;
            if (glyph.width == 2) { cell.ch = 0; cell.continuation = true; row[glyph.column - first.column + 1] = cell; }
        }
        const int x = origin.x + first.column * cw, y = origin.y + first.row * ch;
        cells(x, y, cw, ch, row, columns, clip);
        for (size_t i = begin; i < end; ++i) {
            const auto& glyph = glyphs[i];
            const bool target = glyph.attribute == ATTR_TARGET_CONVERTED || glyph.attribute == ATTR_TARGET_NOTCONVERTED;
            const int left = origin.x + glyph.column * cw;
            const RECT underline{left, y + ch - (target ? 2 : 1), left + glyph.width * cw, y + ch};
            RECT visible{};
            if (IntersectRect(&visible, &underline, &clip)) fill(visible, target ? accent : muted);
        }
        begin = end;
    }
    const int x = origin.x + layout.caretColumn * cw, y = origin.y + layout.caretRow * ch;
    const RECT caret{x, y, x + 2, y + ch}; RECT visible{};
    if (IntersectRect(&visible, &caret, &clip)) fill(visible, accent);
}
void Renderer::line(float x1, float y1, float x2, float y2, COLORREF value, float width) {
    color(value); target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.get(), width);
}
void Renderer::ellipse(const RECT& rect, COLORREF value, float width) {
    color(value); target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((rect.left + rect.right) / 2.0f, (rect.top + rect.bottom) / 2.0f),
        (rect.right - rect.left) / 2.0f, (rect.bottom - rect.top) / 2.0f), brush_.get(), width);
}
void Renderer::outline(const RECT& rect, COLORREF value, float width) {
    if (empty(rect)) return;
    color(value); target_->DrawRectangle(rectangle(rect), brush_.get(), width);
}
void Renderer::icon(Icon icon, const RECT& rect, COLORREF value) {
    if (empty(rect)) return;
    const float size = static_cast<float>(std::min(rect.right - rect.left, rect.bottom - rect.top));
    const float scale = size / 24.0f;
    const float x = (rect.left + rect.right - size) / 2.0f, y = (rect.top + rect.bottom - size) / 2.0f;
    D2D1::Matrix3x2F previous; target_->GetTransform(&previous);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x, y) * previous);
    color(value);
    auto& resources = factories();
    target_->DrawGeometry(resources.icons[static_cast<size_t>(icon)].get(), brush_.get(), 1.6f, resources.iconStroke.get());
    target_->SetTransform(previous);
}
}

void poppy::DropPreview::create(HWND parent, HINSTANCE instance) {
    WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PoppyDropPreview";
    RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, cls.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr, instance, this);
}
void poppy::DropPreview::show(const RECT& rect) {
    SetWindowPos(window_, HWND_TOP, rect.left, rect.top, std::max(0L, rect.right - rect.left), std::max(0L, rect.bottom - rect.top), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}
LRESULT CALLBACK poppy::DropPreview::procedure(HWND window, UINT message, WPARAM a, LPARAM b) {
    auto* self = reinterpret_cast<DropPreview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<DropPreview*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams); self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (self && message == WM_PAINT) {
        self->renderer_.setDpi(GetDpiForWindow(window)); Renderer::Paint paint(self->renderer_, window); if (!paint) return 0;
        RECT rect{}; GetClientRect(window, &rect); self->renderer_.fill(rect, rgb(35, 58, 65)); self->renderer_.outline(rect, accent, 2);
        if (rect.right > 80 && rect.bottom > 40) self->renderer_.label(rect, L"Move tab here", accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return 0;
    }
    return DefWindowProcW(window, message, a, b);
}
