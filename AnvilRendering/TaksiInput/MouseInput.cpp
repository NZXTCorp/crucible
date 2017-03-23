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

bool QuickSelectTimeoutExpired()
{
	if (select_timeout >= clock_::now() || select_timeout == decltype(select_timeout){})
		return false;

	select_timeout = {};
	return true;
}

bool UpdateMouseState(UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (!quick_selecting && select_timeout >= clock_::now() && msg == WM_MBUTTONDOWN) {
		quick_selecting = true;
		select_timeout = {};
		ForgeEvent::StartQuickSelect();
		return true;
	}

	if (!g_bBrowserShowing && !quick_selecting)
		return false;

	if (quick_selecting && !g_bBrowserShowing) {
		POINT pt = { 0, 0 };
		ClientToScreen(g_Proc.m_Stats.m_hWndCap, &pt);
		lParam = POINTTOPOINTS(pt);
	}

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

void UpdateRawMouse(RAWMOUSE &event)
{
	if (!g_bBrowserShowing)
		return;

#if 0
	if (event.usFlags & MOUSE_MOVE_ABSOLUTE)
	{
		mouse_position.x = event.lLastX;
		mouse_position.y = event.lLastY;
	}
	else
	{
		UpdateBounded(mouse_position.x, event.lLastX, 0, g_Proc.m_Stats.m_SizeWnd.cx);
		UpdateBounded(mouse_position.y, event.lLastY, 0, g_Proc.m_Stats.m_SizeWnd.cy);
	}

	auto SendMouse = [&](USHORT flag, WPARAM wParam)
	{
		if (event.usButtonFlags & flag)
			ForgeEvent::MouseEvent(mouse_position.x, mouse_position.y, wParam);
	};

	SendMouse(RI_MOUSE_LEFT_BUTTON_DOWN, WM_LBUTTONDOWN);
	SendMouse(RI_MOUSE_LEFT_BUTTON_UP, WM_LBUTTONUP);
	SendMouse(RI_MOUSE_RIGHT_BUTTON_DOWN, WM_RBUTTONDOWN);
	SendMouse(RI_MOUSE_RIGHT_BUTTON_UP, WM_RBUTTONUP);
	SendMouse(RI_MOUSE_MIDDLE_BUTTON_DOWN, WM_MBUTTONDOWN);
	SendMouse(RI_MOUSE_MIDDLE_BUTTON_UP, WM_MBUTTONUP);
#endif

	ZeroMemory(&event, sizeof(event));
}
