#pragma once

#include <cstdint>

bool UpdateMouseState(UINT msg, WPARAM wParam, LPARAM lParam);

void StartQuickSelectTimeout(uint32_t timeout_seconds, bool from_remote = false);
void StopQuickSelect(bool from_remote = false);
