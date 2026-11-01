#include "ai_status_win.h"
#include "render.h"
#include <chrono>
#include <stdexcept>

namespace poppy {
std::string newSessionId() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot allocate terminal identity");
    std::string result; constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(guid); ++i) { const auto byte = reinterpret_cast<const unsigned char*>(&guid)[i]; result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
void drawAiStatus(Renderer& dc, RECT rect, AiState state, UINT dpi) {
    const int size = MulDiv(14, dpi, 96), x = (rect.left+rect.right)/2, y = (rect.top+rect.bottom)/2;
    const float unit = static_cast<float>(dpi)/96;
    const COLORREF color = state == AiState::Running ? RGB(255,211,64) : state == AiState::Completed ? accent : state == AiState::Idle || state == AiState::Unknown ? muted : RGB(224,192,128);
    const auto line = [&](int x1,int y1,int x2,int y2) { dc.line(x+x1*unit,y+y1*unit,x+x2*unit,y+y2*unit,color,1.6f*unit); };
    if (state == AiState::Running) {
        // Keep the 1.44-second revolution continuous even when a paint is delayed.
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float rotation = static_cast<float>(elapsed % 1440000) * (360.0f / 1440000.0f);
        dc.icon(Renderer::Icon::Spinner, {x-size/2,y-size/2,x-size/2+size,y-size/2+size}, color, rotation);
    } else if (state == AiState::Completed) { line(-6,0,-2,4); line(-2,4,6,-5); }
    else if (state == AiState::Paused) { line(-3,-6,-3,6); line(3,-6,3,6); }
    else if (state == AiState::Idle) dc.fill({x-1,y-1,x+2,y+2},color);
    else if (state == AiState::Unknown) dc.ellipse({x-size/2,y-size/2,x+size/2,y+size/2},color,unit);
    else dc.label(rect, state == AiState::Failed ? L"!" : L"?", color, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}
}
