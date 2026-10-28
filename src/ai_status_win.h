#pragma once
#include "win.h"
#include "ai_status.h"

namespace poppy {
class Renderer;
constexpr UINT wmAiEvent = WM_APP + 30;
std::string newSessionId();
void drawAiStatus(Renderer& renderer, RECT rect, AiState state, UINT dpi, unsigned frame = 0);
}
