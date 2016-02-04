#include "stdafx.h"

#include "MouseInput.h"

#include <Windows.h>

#include "AnvilRendering.h"

extern bool g_bBrowserShowing;

bool UpdateMouseState(unsigned msg, int x, int y, int action)
{
	if (!g_bBrowserShowing)
		return false;

	switch (msg)
	{
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MBUTTONDBLCLK:
	case WM_MOUSEWHEEL:
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
	case WM_XBUTTONDBLCLK:
		ForgeEvent::MouseEvent(x, y, msg);
		return true;
	}

	return false;
}