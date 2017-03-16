#include "stdafx.h"

#include "MouseInput.h"

#include <Windows.h>

#include "AnvilRendering.h"
#include <chrono>

extern bool g_bBrowserShowing;

using clock_ = std::chrono::steady_clock;
static clock_::time_point select_timeout;
bool quick_selecting = false;

bool SendBeginQuickSelectTimeout(uint32_t timeout_ms);
void StartQuickSelectTimeout(uint32_t timeout_ms, bool from_remote)
{
	if (from_remote) {
		SendBeginQuickSelectTimeout(timeout_ms);
		return;
	}

	if (!GetHotKey(HOTKEY_Cancel))
		return;

	using namespace std;
	select_timeout = clock_::now() + chrono::milliseconds{ timeout_ms };
}

bool SendStopQuickSelect();
void StopQuickSelect(bool from_remote)
{
	if (from_remote) {
		SendStopQuickSelect();
		return;
	}

	select_timeout = {};
	quick_selecting = false;
}

bool UpdateMouseState(UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (!quick_selecting && select_timeout >= clock_::now() && msg == WM_MBUTTONDOWN) {
		quick_selecting = true;
		ForgeEvent::StartQuickSelect();
		return true;
	}

	if (!g_bBrowserShowing && !quick_selecting)
		return false;

	switch (msg)
	{
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MBUTTONDBLCLK:
	case WM_MOUSEWHEEL:
		if (g_bBrowserShowing || quick_selecting) {
			ForgeEvent::MouseEvent(msg, wParam, lParam);
			return true;
		}
		break;

	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_RBUTTONDBLCLK:
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
	case WM_XBUTTONDBLCLK:
		if (g_bBrowserShowing) {
			ForgeEvent::MouseEvent(msg, wParam, lParam);
			return true;
		}
	}

	return false;
}