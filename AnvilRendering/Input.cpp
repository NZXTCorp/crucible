#include "stdafx.h"

#include "AnvilRendering.h"

#include "TaksiInput/HotKeys.h"

#include <thread>

using namespace std;

static HWND win = nullptr;

void ToggleOverlay()
{
	hlog(g_bBrowserShowing ? "hiding browser" : "requesting browser");
	g_bBrowserShowing = !g_bBrowserShowing;
}

static void ProcessHotKeys()
{
	for (auto event : g_HotKeys.m_events)
	{
		switch (event.key)
		{
		case HOTKEY_Overlay:
			if (event.event == HKEVENT_PRESS)
				ToggleOverlay();
			break;
		case HOTKEY_Screenshot:
		case HOTKEY_Bookmark:
			break;
		}
	}

	g_HotKeys.m_events.clear();
}

void HandleInputHook(HWND window)
{
	if (!win)
	{
		g_Proc.m_Stats.m_hWndCap = window;
		win = window;
	}

	if (!g_HotKeys.HotkeysAttached())
		g_HotKeys.AttachHotKeysToApp();

	if (g_UserDI.m_bSetup)
		g_UserDI.ProcessDirectInput();
	
	ProcessHotKeys();
}

void StopInputHook()
{
	if (!win)
		return;

	win = nullptr;

	g_HotKeys.DetachHotKeys();
}