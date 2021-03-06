#include "stdafx.h"

#include "AnvilRendering.h"

#include "TaksiInput/HotKeys.h"
#include "TaksiInput/MouseInput.h"
#include "GAPI_render/NewIndicator.h"

#include "../Crucible/IPC.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

using namespace std;

static HWND win = nullptr;

static HHOOK mouse_hook = nullptr;

static void HookMouse()
{
	return;
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

			SetLastError(0);
			if (MapWindowPoints(nullptr, win, &last_pt, 1) || GetLastError() == 0)
			{
				//hlog("mouse stuff: @%d,%d", last_pt.x, last_pt.y);
				ForgeEvent::MouseEvent(last_pt.x, last_pt.y, wParam);
			}

			switch (wParam)
			{
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
						return 1;
			}
		}

		return CallNextHookEx(mouse_hook, code, wParam, lParam);
	}, NULL, thread_id);

	if (mouse_hook)
		hlog("hooked mouse events");
}

static void UpdateBounded(LONG &target, LONG delta, LONG min_, LONG max_)
{
	target = min(max(target + delta, min_), max_);
}

static POINT mouse_position;

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x}

struct ForgeFramebufferServer {
	IPCServer server;

	std::string name;

	atomic<bool> died = true;
	atomic<bool> new_data = false;

	vector<uint8_t> read_data;
	vector<uint8_t> incoming_data;

	mutex share_mutex;
	vector<uint8_t> shared_data;

#pragma optimize("tsg", on)
	void Start()
	{
		static atomic<int> restarts = 0;
		died = false;

		name = "AnvilFramebufferServer" + to_string(GetCurrentProcessId()) + "-" + to_string(restarts++);

		auto expected = g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4;

		server.Start(name, [&](uint8_t *data, size_t size)
		{
			if (!data) {
				died = true;
				hlog("AnvilFramebufferServer: died");
				return;
			}

			if (size != g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4) {
				hlog("AnvilFramebufferServer: got invalid size: %d, expected %d", size, g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4);
				return;
			}

			//hlog("AnvilFramebufferServer: got size %d", size);

			incoming_data.assign(data, data + size);

			{
				LOCK(share_mutex);
				swap(incoming_data, shared_data);
				new_data = true;
			}
		}, expected > 1024 ? expected : -1);
	}

	void Stop()
	{
		server.server.reset();
		died = true;
		new_data = false;

		LOCK(share_mutex);
		read_data.clear();
		incoming_data.clear();
	}
};

static std::array<ProtectedObject<ForgeFramebufferServer>, OVERLAY_COUNT> forgeFramebufferServer;

static const char *name_for_overlay[OVERLAY_COUNT] = {
	"highlighter",
	"streaming",
	"notifications",
};

static array<ForgeEvent::BrowserConnectionDescription, OVERLAY_COUNT> browsers;

void StartFramebufferServer(array<void *, OVERLAY_COUNT> *shared_handles, ForgeEvent::LUID *luid)
{
	for (size_t i = OVERLAY_HIGHLIGHTER; i < OVERLAY_COUNT; i++) {
		browsers[i].shared_handle = shared_handles ? (*shared_handles)[i] : nullptr;
		ForgeEvent::LUID empty{};
		browsers[i].luid = luid ? *luid : empty;
	}

	StartFramebufferServer();
}

void StartFramebufferServer()
{
	for (size_t i = OVERLAY_HIGHLIGHTER; i < OVERLAY_COUNT; i++) {
		browsers[i].name = name_for_overlay[i];

		auto fbs = forgeFramebufferServer[i].Lock();
		if (!fbs)
			continue;

		if (fbs->died)
			fbs->Start();

		browsers[i].server = fbs->name;
	}

	ForgeEvent::InitBrowser(browsers, g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy);
}

void StopFramebufferServer()
{
	for (size_t i = OVERLAY_HIGHLIGHTER; i < OVERLAY_COUNT; i++) {
		browsers[i].name = name_for_overlay[i];

		auto fbs = forgeFramebufferServer[i].Lock();
		if (!fbs)
			continue;

		fbs->Stop();
	}
}

vector<uint8_t> *ReadNewFramebuffer(ActiveOverlay ov)
{
	auto fbs = forgeFramebufferServer[ov].Lock();
	if (!fbs || !fbs->new_data)
		return nullptr;

	{
		LOCK(fbs->share_mutex);
		swap(fbs->shared_data, fbs->read_data);
		fbs->new_data = false;
	}

	return &fbs->read_data;
}

extern void ResetOverlayCursor();

void DisableRawInput();
void RestoreRawInput();
void RestoreCursor();

bool SendDismissOverlay();
void DismissOverlay(bool from_remote)
{
	if (!g_bBrowserShowing)
		return;

	if (from_remote && SendDismissOverlay())
		return;

	ForgeEvent::HideBrowser();
	hlog("Hiding browser");
	//forgeFramebufferServer.Stop();

	RestoreCursor();
	RestoreRawInput();

	g_bBrowserShowing = false;

	if (g_Proc.m_Stats.m_hWndCap && !from_remote)
		SetWindowPos(g_Proc.m_Stats.m_hWndCap, 0, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

ActiveOverlay GetOverlayFromName(const string &name)
{
	for (size_t i = OVERLAY_HIGHLIGHTER; i < OVERLAY_COUNT; i++) {
		if (name_for_overlay[i] == name)
			return static_cast<ActiveOverlay>(i);
	}

	return OVERLAY_COUNT;
}

void DismissNamedOverlay(const string &name)
{
	for (size_t i = OVERLAY_HIGHLIGHTER; i < OVERLAY_COUNT; i++)
	{
		if (active_overlay != static_cast<ActiveOverlay>(i))
			continue;

		if (name_for_overlay[i] != name)
			continue;

		hlog("Hiding named overlay '%s' from remote", name.c_str());
		DismissOverlay(true);
	}
}

void OverlayUnclipCursor();
void ShowOverlayCursor();

void ToggleOverlay(ActiveOverlay overlay)
{
	auto fbs = forgeFramebufferServer[overlay].Lock();
	if (g_bBrowserShowing && fbs && fbs->died)
		fbs->Start();

	if (!g_bBrowserShowing || active_overlay != overlay) {
		if (!fbs) {
			hlog("Requested browser for overlay %d ignored because browser could not be locked", overlay);
			return;
		}

		if (fbs->died)
			fbs->Start();

		if (overlay != active_overlay)
			ForgeEvent::HideBrowser();

		browsers[overlay].name = name_for_overlay[overlay];
		browsers[overlay].server = fbs->name;

		active_overlay = overlay;
		ForgeEvent::ShowBrowser(browsers[overlay], g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy);
		hlog("Requesting browser");

		bool browser_was_active = g_bBrowserShowing;
		g_bBrowserShowing = true;

		if (!browser_was_active)
		{
			DisableRawInput();
			ShowOverlayCursor();
			OverlayUnclipCursor();
		}

		mouse_position.x = g_Proc.m_Stats.m_SizeWnd.cx / 2;
		mouse_position.y = g_Proc.m_Stats.m_SizeWnd.cy / 2;
	} else {
		DismissOverlay(false);
	}
}

static void ProcessHotKeys()
{
	for (auto event : g_HotKeys.m_events)
	{
		switch (event.key)
		{
		case HOTKEY_Overlay:
			if (event.event == HKEVENT_PRESS && !TutorialLockStatus())
				ToggleOverlay(OVERLAY_HIGHLIGHTER);
			break;
		case HOTKEY_QuickClip:
			if (event.event == HKEVENT_PRESS) {
				ForgeEvent::HideTutorial();
				ForgeEvent::SaveQuickClip(TutorialLockStatus());
				SetTutorialLock(false);
			}
			break;
		case HOTKEY_QuickForwardClip:
			if (event.event == HKEVENT_PRESS && !TutorialLockStatus())
				ForgeEvent::ToggleQuickForwardClip();
			break;
		case HOTKEY_Bookmark:
			if (event.event == HKEVENT_PRESS && !TutorialLockStatus())
				ForgeEvent::CreateBookmark();
			break;
		case HOTKEY_Screenshot:
			if (event.event == HKEVENT_PRESS && !TutorialLockStatus())
				ForgeEvent::SaveScreenshot();
			break;
		case HOTKEY_Stream:
			if (event.event == HKEVENT_PRESS && !TutorialLockStatus())
				ToggleOverlay(OVERLAY_NOTIFICATIONS);
			break;
		case HOTKEY_StartStopStream:
			if (event.event == HKEVENT_PRESS && !TutorialLockStatus())
				ForgeEvent::StartStopStream();
			break;
		}
	}

	g_HotKeys.m_events.clear();
}

static bool hwnd_sent = false;

void HookWndProc();
void HandleInputHook(HWND window)
{
#ifndef ANVIL_HOTKEYS
	return;
#endif

	if (!win)
	{
		g_Proc.m_Stats.m_hWndCap = window;
		win = window;
		//HookWndProc();
		hwnd_sent = false;
	}

	if (g_Proc.m_Stats.m_hWndCap && !hwnd_sent) {
		hwnd_sent = ForgeEvent::SetGameHWND((GetWindow(g_Proc.m_Stats.m_hWndCap, GW_OWNER) == NULL) ? g_Proc.m_Stats.m_hWndCap : GetWindow(g_Proc.m_Stats.m_hWndCap, GW_OWNER));
	}

	if (!g_HotKeys.HotkeysAttached())
		g_HotKeys.AttachHotKeysToApp();

	// TODO: Remove and implement DI hooks instead
	/*if (g_UserDI.m_bSetup)
		g_UserDI.ProcessDirectInput();*/

	HookMouse();

	ProcessHotKeys();

	ResetOverlayCursor();
	
	if (QuickSelectTimeoutExpired())
		ForgeEvent::QuickSelectTimeoutExpired();
}

void StopInputHook()
{
	if (!win)
		return;

	win = nullptr;

	g_HotKeys.DetachHotKeys();
	UnhookWindowsHookEx(mouse_hook);
}