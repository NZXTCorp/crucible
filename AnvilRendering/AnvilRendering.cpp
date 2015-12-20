// AnvilRendering.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"

#include "../Crucible/IPC.hpp"

#include "AnvilRendering.h"
#include "GAPI_render/NewIndicator.h"

#include <json/reader.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <d3dcompiler.h>

using namespace std;

struct FreeHandle
{
	void operator()(HANDLE h)
	{
		CloseHandle(h);
	}
};

using Handle = unique_ptr<void, FreeHandle>;

void (*hlog)(const char *fmt, ...) = nullptr;


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
	func(currentIndicator, 255);
}

namespace CrucibleCommand {

using namespace json;

static void HandleCommands(uint8_t *data, size_t size)
{
	if (!data)
		return;
	
	try {
		Object obj;

		hlog("Anvil got: '%s'", data);
		istringstream ss{{data, data + size - 1}};
		Reader::Read(obj, ss);

		auto cmd = static_cast<String>(obj["command"]).Value();

		if (!cmd.length())
			return hlog("Got invalid command with 0 length");

		hlog("Got unknown command '%s' (%d)", cmd.c_str(), cmd.length());

	} catch (Exception &e) {
		hlog("Unable to process command in HandleCommands: %s", e.what());
	}
}

}

ProcessCompat g_Proc;

pD3DCompile s_D3DCompile = nullptr;

IndicatorManager indicatorManager;

IPCServer crucibleConnection;

ULONG_PTR gdi_token = 0;

C_EXPORT bool overlay_init(void (*hlog_)(const char *fmt, ...))
{
	hlog = hlog_;
	hlog("Started overlay");

	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::Status status = Gdiplus::GdiplusStartup(&gdi_token, &gdiplusStartupInput, NULL);

	indicatorManager.LoadImages();

	crucibleConnection.Start("AnvilRenderer" + to_string(GetCurrentProcessId()), CrucibleCommand::HandleCommands);

	return true;
}

void overlay_d3d11_free();
void overlay_d3d10_free();
void overlay_d3d9_free();
void overlay_gl_free();

C_EXPORT void overlay_free()
{
	overlay_d3d11_free();
	overlay_d3d10_free();
	overlay_d3d9_free();
	overlay_gl_free();

	if (gdi_token)
		Gdiplus::GdiplusShutdown(gdi_token);
	hlog("Stopped overlay");
}

C_EXPORT void overlay_compile_dxgi_shaders(pD3DCompile compile)
{
	s_D3DCompile = compile;
}
