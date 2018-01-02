#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>
#include "../Crucible/ProtectedObject.hpp"

#define ANVIL_HOTKEYS

#ifndef ANVIL_PIPE_NAME_SUFFIX
#define ANVIL_PIPE_NAME_SUFFIX ""
#endif

#define ANVIL_PIPE_NAME "AnvilRenderer" ANVIL_PIPE_NAME_SUFFIX

#define C_EXPORT extern "C" __declspec(dllexport)

extern void (*hlog)(const char *fmt, ...);

#define LOG_MSG(x, ...) hlog(x, __VA_ARGS__)
#define LOG_WARN LOG_MSG
#define LOG_CR

#ifdef _DEBUG
#define DEBUG_MSG LOG_MSG
#else
#define DEBUG_MSG(...)
#endif

#define DEBUG_TRACE(...)
#define DEBUG_ERR(...)

struct ProcessCompat {
	struct ProcStatsCompat {
		SIZE m_SizeWnd{};					// the window/backbuffer size. (pixels)
		HWND m_hWndCap{};
	} m_Stats;
};

extern ProcessCompat g_Proc;

class IndicatorManager;
extern IndicatorManager indicatorManager;

enum IndicatorEvent;
extern IndicatorEvent currentIndicator;
extern bool show_notifications;

template <typename Fun>
void ShowCurrentIndicator(Fun &&func)
{
	if (currentIndicator >= INDICATE_NONE)
		return;

	func(currentIndicator, 255);
}

void HandleInputHook(HWND window);


enum HOTKEY_TYPE
{
	HOTKEY_Screenshot,
	HOTKEY_Bookmark,
	HOTKEY_Overlay,
	HOTKEY_Stream,
	HOTKEY_StartStopStream,
	HOTKEY_PTT,
	HOTKEY_QuickClip,
	HOTKEY_QuickForwardClip,
	HOTKEY_Cancel,
	HOTKEY_Select,
	HOTKEY_Accept,
	HOTKEY_Decline,
	HOTKEY_QTY,
};

static inline const char *HotKeyTypeName(HOTKEY_TYPE hotkey)
{
	switch (hotkey)
	{
#define HK(x) case x: return #x
		HK(HOTKEY_Screenshot);
		HK(HOTKEY_Bookmark);
		HK(HOTKEY_Overlay);
		HK(HOTKEY_Stream);
		HK(HOTKEY_StartStopStream);
		HK(HOTKEY_QuickClip);
		HK(HOTKEY_QuickForwardClip);
	default:
		break;
	}

	return "HOTKEY_unknown";
}

enum HOTKEY_EVENT
{
	HKEVENT_PRESS,
	HKEVENT_RELEASE,
	HKEVENT_QTY
};

bool HotkeyModifiersMatch(HOTKEY_TYPE t, BYTE modifiers);
BYTE GetHotKey(HOTKEY_TYPE t);

enum ActiveOverlay
{
	OVERLAY_HIGHLIGHTER,
	OVERLAY_STREAMING,
	OVERLAY_NOTIFICATIONS,
	OVERLAY_COUNT
};

extern ActiveOverlay active_overlay;
void ToggleOverlay(const ActiveOverlay overlay);

ActiveOverlay GetOverlayFromName(const std::string &name);

extern HINSTANCE g_hInst;
extern bool g_bUseDirectInput;
extern bool g_bUseKeyboard;
extern bool g_bBrowserShowing;

namespace ForgeEvent
{
	struct LUID {
		uint32_t low = 0;
		int32_t high = 0;

		bool operator==(const LUID &other)
		{
			return low == other.low && high == other.high;
		}
	};

	struct BrowserConnectionDescription {
		std::string server;
		const char *name;
		void *shared_handle = nullptr;

		LUID luid;
	};

	bool KeyEvent(UINT msg, WPARAM wParam, LPARAM lParam);
	bool MouseEvent(UINT msg, WPARAM wParam, LPARAM lParam);
	bool InitBrowser(const std::array<BrowserConnectionDescription, OVERLAY_COUNT> &browsers, LONG width, LONG height);
	bool ShowBrowser(const BrowserConnectionDescription &server, LONG width, LONG height);
	bool HideBrowser();
	bool SetGameHWND(HWND hwnd);
	bool HideTutorial();
	bool CreateBookmark();
	bool SaveScreenshot();
	bool StartStopStream(); // For the new Start/Stop stream hotkey
	bool StartStream();
	bool StopStream();
	bool SaveQuickClip(bool tutorial_active=false);
	bool ToggleQuickForwardClip();
	bool DismissQuickSelect();
	bool StartQuickSelect();
	bool QuickSelectTimeoutExpired();
	bool SendAccept();
	bool SendDecline();
}

std::vector<uint8_t> *ReadNewFramebuffer(ActiveOverlay ov);
void StartFramebufferServer(std::array<void *, OVERLAY_COUNT> *shared_handles, ForgeEvent::LUID *luid);
void StartFramebufferServer();

struct SharedTextureDesc {
	void *shared_handle = nullptr;
	ForgeEvent::LUID luid;
};

extern ProtectedObject<std::array<SharedTextureDesc, OVERLAY_COUNT>> incompatible_shared_textures;


extern bool (*d3d9_create_shared_tex)(UINT, UINT, DWORD, void**, void**);
extern bool (*d3d9_luid)(void*);
