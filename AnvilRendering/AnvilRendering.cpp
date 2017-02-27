// AnvilRendering.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"

#include "../Crucible/IPC.hpp"
#include "../Crucible/ProtectedObject.hpp"
#include "../Crucible/ThreadTools.hpp"

#include "AnvilRendering.h"
#include "GAPI_render/NewIndicator.h"

#include <json/reader.h>
#include <json/writer.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <commctrl.h>	// HOTKEYF_ALT
#include <d3dcompiler.h>

using namespace std;

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

struct FreeHandle
{
	void operator()(HANDLE h)
	{
		CloseHandle(h);
	}
};

using Handle = unique_ptr<void, FreeHandle>;

void (*hlog)(const char *fmt, ...) = nullptr;


namespace ForgeEvent
{
	using namespace json;

	IPCClient forge_client;
	std::string current_connection;

	static bool SendEvent(const Object &object)
	{
		ostringstream sstr;
		Writer::Write(object, sstr);
		return forge_client.Write(sstr.str());
	}

	static Object EventCreate(const char *name)
	{
		return Object().Set("event", String(name));
	}

	bool KeyEvent(UINT msg, WPARAM wParam, LPARAM lParam)
	{
		return SendEvent(EventCreate("key_event")
			.Set("msg", msg)
			.Set("wParam", wParam)
			.Set("lParam", lParam));
	}

	bool MouseEvent(UINT msg, WPARAM wParam, LPARAM lParam)
	{
		return SendEvent(EventCreate("mouse_event")
			.Set("msg", msg)
			.Set("wParam", Number(wParam))
			.Set("lParam", lParam));
	}

	bool InitBrowser(const std::array<BrowserConnectionDescription, OVERLAY_COUNT> &browsers, LONG width, LONG height)
	{
		Array servers;
		for (const auto &browser : browsers)
			servers.Append(Object()
				.Set("server", browser.server)
				.Set("name", browser.name));

		return SendEvent(EventCreate("init_browser")
			.Set("servers", servers)
			.Set("width", Number(width))
			.Set("height", Number(height)));
	}

	bool ShowBrowser(const BrowserConnectionDescription &server, LONG width, LONG height)
	{
		return SendEvent(EventCreate("show_browser")
			.Set("framebuffer_server", String(server.server))
			.Set("width", Number(width))
			.Set("height", Number(height))
			.Set("name", String(server.name)));
	}

	bool SetGameHWND(HWND hwnd)
	{
		return SendEvent(EventCreate("set_game_hwnd")
			.Set("hwnd", (intptr_t)hwnd));
	}

	bool HideBrowser()
	{
		return SendEvent(EventCreate("hide_browser"));
	}

	bool HideTutorial()
	{
		return SendEvent(EventCreate("hide_tutorial"));
	}

	bool CreateBookmark()
	{
		return SendEvent(EventCreate("create_bookmark"));
	}

	bool SaveScreenshot()
	{
		return SendEvent(EventCreate("save_screenshot"));
	}

	bool SaveQuickClip(bool tutorial_active)
	{
		return SendEvent(EventCreate("save_quick_clip")
			.Set("tutorial_active", tutorial_active));
	}

	bool SaveQuickForwardClip()
	{
		return SendEvent(EventCreate("save_quick_forward_clip"));
	}

	bool StartStopStream()
	{
		return SendEvent(EventCreate("start_stop_stream_hotkey"));
	}

	bool StartStream()
	{
		return SendEvent(EventCreate("start_stream"));
	}

	bool StopStream()
	{
		return SendEvent(EventCreate("stop_stream"));
	}
}


TAKSI_INDICATE_TYPE m_eSquareIndicator; // state for the old square indicator

										// new indicator stuff, we should really wrap it up in a separate object
IndicatorEvent m_eIndicatorEvent; // current indicator event we're displaying
IndicatorEvent m_eIndicatorContinuousEvent; // if we were showing a continuous notification like 'streaming', it gets saved here so we can return to it after any timeout notifications like screenshots
unsigned char m_uIndicatorAlpha;  // 0-255 alpha value for indicator fadeout and pulsating effects
IndicatorAnimation m_eIndicatorAnimation; // animation effect we're using
ULONGLONG m_ulIndicatorEventStart; // time we started current indicator event
ULONGLONG m_ulIndicatorEventStop; // time we should stop current indicator event
ULONGLONG m_ulIndicatorLastUpdate; // time we last updated current indicator event


static IndicatorEvent currentIndicator = INDICATE_NONE;

void ShowCurrentIndicator(const std::function<void(IndicatorEvent, BYTE /*alpha*/)> &func)
{
	if (currentIndicator >= INDICATE_NONE)
		return;

	func(currentIndicator, 255);
}

mutex hotkeys_mutex;
static struct {
	BYTE modifier_whitelist;
	BYTE modifier_blacklist;
	BYTE virtual_key;
} hotkeys[HOTKEY_QTY] = { 0 };

ProtectedObject<HCURSOR> overlay_cursor;

extern void DismissOverlay(bool);
extern void DismissNamedOverlay(const string &name);

static void RestartCrucibleServer();

namespace CrucibleCommand {

using namespace json;

static void HandleIndicatorCommand(Object &obj)
{
	static const map<string, IndicatorEvent> indicators = {
		{"idle",       INDICATE_NONE},
		{"capturing",  INDICATE_CAPTURING},
		{"mic_idle",   INDICATE_MIC_IDLE},
		{"mic_active", INDICATE_MIC_ACTIVE},
		{"mic_muted",  INDICATE_MIC_MUTED},
		{"enabled",    INDICATE_ENABLED},
		{"bookmark",   INDICATE_BOOKMARK},
		{"cache_limit",INDICATE_CACHE_LIMIT},
		{"clip_processing", INDICATE_CLIP_PROCESSING},
		{"clip_processed", INDICATE_CLIP_PROCESSED},
		{"stream_started", INDICATE_STREAM_STARTED},
		{"stream_stopped", INDICATE_STREAM_STOPPED},
		{"streaming", INDICATE_STREAMING},
		{"stream_mic_idle", INDICATE_STREAM_MIC_IDLE},
		{"stream_mic_active", INDICATE_STREAM_MIC_ACTIVE},
		{"stream_mic_muted", INDICATE_STREAM_MIC_MUTED},
		{"screenshot_processing", INDICATE_SCREENSHOT_PROCESSING},
		{"screenshot", INDICATE_SCREENSHOT_SAVED},
		{"first_time_tutorial", INDICATE_TUTORIAL},
		{"forward_buffer_in_progress", INDICATE_FORWARD_BUFFER},
	};

	auto indicator = static_cast<String>(obj["indicator"]).Value();

	auto elem = indicators.find(indicator);
	if (elem == end(indicators))
		return hlog("Got invalid indicator '%s'", indicator.c_str());

	if (indicator == "first_time_tutorial")
		SetTutorialLock(true);

	currentIndicator = elem->second;
}

static void DisableIndicators(Object &obj)
{
	bool disable = Boolean(obj["disable_indicators"]);

	indicatorManager.DisableIndicators(disable);
}

static void HandleForgeInfo(Object &obj)
{
	String event = obj["anvil_event"];
	if (event.Value().empty())
		return hlog("Got empty anvil_event name via forge_info");

	if (event.Value() != ForgeEvent::current_connection)
		ForgeEvent::forge_client.Open(event.Value());

	StartFramebufferServer();
}

static void HandleUpdateSettings(Object &obj)
{
	auto UpdateHotkey = [&](HOTKEY_TYPE hotkey, const char *setting_name)
	{
		Object key_data = obj[setting_name];
		if (!key_data.HasMember("keycode")) {
			hotkeys[hotkey] = { 0 };
			hlog("hotkey '%s' (%s) disabled", HotKeyTypeName(hotkey), setting_name);
			return;
		}

		Boolean meta = key_data["meta"];
		if (meta)
			hlog("meta modifier not supported for hotkey '%s' (%s)", HotKeyTypeName(hotkey), setting_name);

		Boolean shift = key_data["shift"];
		Boolean ctrl = key_data["ctrl"];
		Boolean alt = key_data["alt"];
		Number code = key_data["keycode"];

		hotkeys[hotkey].modifier_whitelist = ((shift ? HOTKEYF_SHIFT : 0) |
			(ctrl ? HOTKEYF_CONTROL : 0) |
			(alt ? HOTKEYF_ALT : 0));

		hotkeys[hotkey].virtual_key = static_cast<BYTE>(code.Value());

		if (!hotkeys[hotkey].virtual_key)
			return;

		SetIndicatorHotkey(hotkey, static_cast<int>(code.Value()), ctrl, alt, shift);

		hlog("hotkey '%s' (%s) updated", HotKeyTypeName(hotkey), setting_name);
	};

	{
		LOCK(hotkeys_mutex);
		UpdateHotkey(HOTKEY_Bookmark, "bookmark_key");
		UpdateHotkey(HOTKEY_Overlay, "highlight_key");
		UpdateHotkey(HOTKEY_Stream, "stream_key");
		UpdateHotkey(HOTKEY_StartStopStream, "start_stop_stream_key");
		UpdateHotkey(HOTKEY_PTT, "ptt_key");
		UpdateHotkey(HOTKEY_Screenshot, "screenshot_key");
		UpdateHotkey(HOTKEY_QuickClip, "quick_clip_key");
		UpdateHotkey(HOTKEY_QuickForwardClip, "quick_clip_forward_key");

		indicatorManager.UpdateImages();

		for (size_t i = 0; i < HOTKEY_QTY; i++) {
			auto &hk = hotkeys[i];
			if (!hk.virtual_key)
				continue;

			hk.modifier_blacklist = 0;
			for (size_t j = 0; j < HOTKEY_QTY; j++) {
				auto other_hk = hotkeys[j];
				if (i == j || !other_hk.virtual_key || hk.virtual_key != other_hk.virtual_key)
					continue;

				hk.modifier_blacklist |= other_hk.modifier_whitelist & ~hk.modifier_whitelist;
			}
		}
	}
}

static void HandleSetCursor(Object &obj)
{
	auto id = MAKEINTRESOURCEW(Number(obj["cursor"]).Value());
	if (!id)
		id = IDC_ARROW;

	auto handle = LoadCursorW((id >= IDC_ARROW) ? nullptr : g_hInst, MAKEINTRESOURCEW(id));

	auto cursor = overlay_cursor.Lock();
	*cursor = handle;
}

static void HandleStreamStatus(Object &)
{
}

static void HandleDismissOverlay(Object &obj)
{
	DismissNamedOverlay(String(obj["name"]));
}

static void HandleForwardBufferIndicatorUpdate(Object &obj)
{
	auto msg = obj.Maybe()["text"].As<String>();
	indicatorManager.UpdateForwardBufferText(msg ? msg->ValueW() : wstring());
}

static void HandleCommands(uint8_t *data, size_t size)
{
	static const map<string, void(*)(Object&)> handlers = {
		{ "indicator", HandleIndicatorCommand },
		{ "disable_native_indicators", DisableIndicators },
		{ "forge_info", HandleForgeInfo },
		{ "update_settings", HandleUpdateSettings },
		{ "set_cursor", HandleSetCursor },
		{ "dismiss_overlay", HandleDismissOverlay },
		{ "stream_status", HandleStreamStatus },
		{ "update_forward_buffer_indicator", HandleForwardBufferIndicatorUpdate },
	};

	if (!data) {
		hlog("AnvilRender: command connection died");
		RestartCrucibleServer();
		return;
	}
	
	try {
		Object obj;

		hlog("Anvil got: '%s'", data);
		istringstream ss{{data, data + size - 1}};
		Reader::Read(obj, ss);

		auto cmd = static_cast<String>(obj["command"]).Value();

		if (!cmd.length())
			return hlog("Got invalid command with 0 length");

		auto handler = handlers.find(cmd);
		if (handler != cend(handlers))
			handler->second(obj);
		else
			hlog("Got unknown command '%s' (%d)", cmd.c_str(), cmd.length());

	} catch (Exception &e) {
		hlog("Unable to process command in HandleCommands: %s", e.what());
	}
}

}

ProcessCompat g_Proc;

pD3DCompile s_D3DCompile = nullptr;

IndicatorManager indicatorManager;

shared_ptr<void> crucibleConnectionRestartEvent;
IPCServer crucibleConnection;
JoiningThread crucibleConnectionRestartThread;

ULONG_PTR gdi_token = 0;

HINSTANCE g_hInst = nullptr;
bool g_bUseDirectInput = true;
bool g_bUseKeyboard = true;
bool g_bBrowserShowing = false;

ActiveOverlay active_overlay = OVERLAY_HIGHLIGHTER;

bool HotkeyModifiersMatch(HOTKEY_TYPE t, BYTE modifiers)
{
	if (t >= 0 && t < HOTKEY_QTY)
	{
		LOCK(hotkeys_mutex);
		return (modifiers & hotkeys[t].modifier_whitelist) == hotkeys[t].modifier_whitelist &&
			(modifiers & hotkeys[t].modifier_blacklist) == 0;
	}
	return false;
}

BYTE GetHotKey(HOTKEY_TYPE t)
{
	if (t >= 0 && t < HOTKEY_QTY)
	{
		LOCK(hotkeys_mutex);
		return hotkeys[t].virtual_key;
	}
	return 0;
}

static bool StartCrucibleServer()
{
	return crucibleConnection.Start("AnvilRenderer" + to_string(GetCurrentProcessId()), CrucibleCommand::HandleCommands);
}

static void RestartCrucibleServer()
{
	currentIndicator = INDICATE_NONE;
	{
		LOCK(hotkeys_mutex);
		for (int t = 0; t < HOTKEY_QTY; t++)
			hotkeys[t] = { 0 };
	}
	DismissOverlay(true);
	*overlay_cursor.Lock() = LoadCursorW(nullptr, IDC_ARROW);

	if (!crucibleConnectionRestartEvent)
		return;

	SetEvent(crucibleConnectionRestartEvent.get());
}

static void CreateRestartThread()
{
	crucibleConnectionRestartEvent.reset(CreateEvent(nullptr, false, false, nullptr), HandleDeleter{});

	shared_ptr<void> ev{ CreateEvent(nullptr, true, false, nullptr), HandleDeleter{} };

	crucibleConnectionRestartThread.make_joinable = [ev] { SetEvent(ev.get()); };

	crucibleConnectionRestartThread.t = thread([ev]
	{
		HANDLE objs[] = { ev.get(), crucibleConnectionRestartEvent.get() };
		for (;;)
		{
			auto res = WaitForMultipleObjects(2, objs, false, INFINITE);
			if (res != WAIT_OBJECT_0 + 1)
				break;

			if (StartCrucibleServer())
				hlog("AnvilRender: command connection restarted");
		}
	});
}

C_EXPORT bool overlay_init(void (*hlog_)(const char *fmt, ...))
{
	hlog = hlog_;
	hlog("Started overlay");

	GetModuleHandleEx(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCTSTR)overlay_init, &g_hInst);

	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::GdiplusStartupOutput gdiplusStartupOutput;
	ULONG_PTR gdiplusToken;
	GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, &gdiplusStartupOutput);

	indicatorManager.LoadImages();

	*overlay_cursor.Lock() = LoadCursorW(nullptr, IDC_ARROW);

	CreateRestartThread();

	StartCrucibleServer();

	return true;
}

void StopInputHook();
void overlay_d3d11_free();
void overlay_d3d10_free();
void overlay_d3d9_free();
void overlay_gl_free();

C_EXPORT void overlay_reset()
{
	StopInputHook();

	overlay_d3d11_free();
	overlay_d3d10_free();
	overlay_d3d9_free();
	overlay_gl_free();
}

C_EXPORT void overlay_free()
{
	StopInputHook();

	if (gdi_token)
		Gdiplus::GdiplusShutdown(gdi_token);
	hlog("Stopped overlay");
}

C_EXPORT void overlay_compile_dxgi_shaders(pD3DCompile compile)
{
	s_D3DCompile = compile;
}
