#include "ai_status_win.h"
#include "render.h"
#include <cmath>
#include <stdexcept>

namespace poppy {
std::string newSessionId() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot allocate terminal identity");
    std::string result; constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(guid); ++i) { const auto byte = reinterpret_cast<const unsigned char*>(&guid)[i]; result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
void drawAiStatus(Renderer& dc, RECT rect, AiState state, UINT dpi, unsigned frame) {
    const int size = MulDiv(14, dpi, 96), x = (rect.left+rect.right)/2, y = (rect.top+rect.bottom)/2;
    const float unit = static_cast<float>(dpi)/96;
    const COLORREF color = state == AiState::Running || state == AiState::Completed ? accent : state == AiState::Idle || state == AiState::Unknown ? muted : RGB(224,192,128);
    const auto line = [&](int x1,int y1,int x2,int y2) { dc.line(x+x1*unit,y+y1*unit,x+x2*unit,y+y2*unit,color,1.6f*unit); };
    if (state == AiState::Running) {
        dc.ellipse({x-size/2,y-size/2,x+size/2,y+size/2}, muted, unit);
        const float angle = frame * 0.35f;
        dc.line(static_cast<float>(x),static_cast<float>(y),x+std::cos(angle)*size/2,y+std::sin(angle)*size/2,color,2*unit);
    } else if (state == AiState::Completed) { line(-6,0,-2,4); line(-2,4,6,-5); }
    else if (state == AiState::Paused) { line(-3,-6,-3,6); line(3,-6,3,6); }
    else if (state == AiState::Idle) dc.fill({x-1,y-1,x+2,y+2},color);
    else if (state == AiState::Unknown) dc.ellipse({x-size/2,y-size/2,x+size/2,y+size/2},color,unit);
    else dc.label(rect, state == AiState::Failed ? L"!" : L"?", color, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}
}
