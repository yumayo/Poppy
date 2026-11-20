#pragma once
#include "win.h"
#include "ai_status.h"

namespace poppy {
class Renderer;
constexpr UINT wmAiEvent = WM_APP + 30;
std::string newSessionId();
void drawAiStatus(Renderer& renderer, RECT rect, AiState state, UINT dpi);
// Matches the taskbar's unread badge.
constexpr COLORREF unreadColor = RGB(0xE5, 0x1A, 0x1A);
// A small red dot on the top-right of the status icon drawn in the same rect.
void drawUnreadDot(Renderer& renderer, RECT rect, UINT dpi);
}
