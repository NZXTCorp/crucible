#include "stdafx.h"

#include "AnvilRendering.h"

#include "TaksiInput/HotKeys.h"

#include <thread>

using namespace std;

static HWND win = nullptr;

static HHOOK mouse_hook = nullptr;

static void HookMouse()
{
	if (mouse_hook)
		return;

	auto thread_id = GetWindowThreadProcessId(win, nullptr);

	mouse_hook = SetWindowsHookEx(WH_MOUSE, [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT
	{
		POINT last_pt;

		if (g_bBrowserShowing && code == HC_ACTION)
		{
			auto &mhs = *reinterpret_cast<MOUSEHOOKSTRUCT*>(lParam);
			last_pt = mhs.pt;
			if (MapWindowPoints(nullptr, win, &last_pt, 1))
				hlog("mouse stuff: @%d,%d", last_pt.x, last_pt.y);
			return 1;
		}

		return CallNextHookEx(mouse_hook, code, wParam, lParam);
	}, NULL, thread_id);

	if (mouse_hook)
		hlog("hooked mouse events");
}

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

	HookMouse();

	ProcessHotKeys();
}

void StopInputHook()
{
	if (!win)
		return;

	win = nullptr;

	g_HotKeys.DetachHotKeys();
	UnhookWindowsHookEx(mouse_hook);
}