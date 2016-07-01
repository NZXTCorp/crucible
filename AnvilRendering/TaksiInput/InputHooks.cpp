// [InputHooks.cpp 2014-03-12 abright]
// new home for input hooking code

#include "stdafx.h"

#include "TaksiInput.h"

#include "InputHooks.h"
#include "KeyboardInput.h"
#include "MouseInput.h"

#include <vector>

#include "../../Crucible/ProtectedObject.hpp"

#define HOOK_REGISTER_RAW_DEVICES

#ifdef USE_DIRECTI
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

enum DIDEVICE8_FUNC_TYPE {
	DI8_DEVICE_QueryInterface = 0,
	DI8_DEVICE_AddRef = 1,
	DI8_DEVICE_Release = 2,

	DI8_DEVICE_Acquire = 7,
	DI8_DEVICE_Unacquire = 8,
	DI8_DEVICE_GetDeviceState = 9,
	DI8_DEVICE_GetDeviceData = 10
};

static UINT_PTR s_nDI8_GetDeviceState = 0;
static UINT_PTR s_nDI8_GetDeviceData = 0;

typedef HRESULT (WINAPI *DIRECTINPUT8CREATE)(HINSTANCE, DWORD, REFIID, LPVOID, IDirectInput8**);
static DIRECTINPUT8CREATE s_DirectInput8Create = NULL;

typedef HRESULT (WINAPI *GETDEVICESTATE)(IDirectInputDevice8 *, DWORD, LPVOID);
static GETDEVICESTATE s_DI8_GetDeviceState = NULL;

typedef HRESULT (WINAPI *GETDEVICEDATA)(IDirectInputDevice8 *, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD , DWORD);
static GETDEVICEDATA s_DI8_GetDeviceData = NULL;

static CHookJump s_HookDeviceState;
static CHookJump s_HookDeviceData;

#endif

typedef BOOL (WINAPI *GETKEYBOARDSTATE)(PBYTE);
static GETKEYBOARDSTATE s_GetKeyboardState = NULL;

typedef SHORT (WINAPI *GETASYNCKEYSTATE)(int);
static GETASYNCKEYSTATE s_GetAsyncKeyState = NULL;

typedef BOOL (WINAPI *GETCURSORPOS)(LPPOINT);
static GETCURSORPOS s_GetCursorPos = NULL;

typedef BOOL (WINAPI *SETCURSORPOS)(INT, INT);
static SETCURSORPOS s_SetCursorPos = NULL;

typedef UINT (WINAPI *GETRAWINPUTDATA)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GETRAWINPUTDATA s_GetRawInputData = NULL;

typedef UINT (WINAPI *GETRAWINPUTBUFFER)(PRAWINPUT, PUINT, UINT);
static GETRAWINPUTBUFFER s_GetRawInputBuffer = NULL;

typedef UINT (WINAPI *GETREGISTEREDRAWINPUTDEVICES)(PRAWINPUTDEVICE, PUINT, UINT);
static GETREGISTEREDRAWINPUTDEVICES s_GetRegisteredRawInputDevices = nullptr;

typedef BOOL (WINAPI *REGISTERRAWINPUTDEVICES)(PCRAWINPUTDEVICE, UINT, UINT);
static REGISTERRAWINPUTDEVICES s_RegisterRawInputDevices = nullptr;

typedef WINUSERAPI HCURSOR (WINAPI *SETCURSOR)(HCURSOR hCursor);
static SETCURSOR s_SetCursor = nullptr;

typedef WINUSERAPI HCURSOR (WINAPI *GETCURSOR)(VOID);
static GETCURSOR s_GetCursor = nullptr;

static CHookJump s_HookGetKeyboardState;
static CHookJump s_HookGetAsyncKeyState;
static CHookJump s_HookGetCursorPos;
static CHookJump s_HookSetCursorPos;
static CHookJump s_HookGetRawInputData;
static CHookJump s_HookGetRawInputBuffer;

static CHookJump s_HookGetCursor;

#define DECLARE_HOOK_EXP(func, name, wrapper) static FuncHook<decltype(func)> name{ #func, (decltype(func)*)wrapper }
#define DECLARE_HOOK(func, wrapper) DECLARE_HOOK_EXP(func, s_Hook ## func, wrapper)
#define DECLARE_HOOK_EX_(func, name) static FuncHook<decltype(func)> name = FuncHook<decltype(func)>{ #func, nullptr } + []
#define DECLARE_HOOK_EX(func) DECLARE_HOOK_EX_(func, s_Hook ## func)

static struct cursor_info_ {
	bool saved = false;
	bool showing = false;
} cursor_info;

DECLARE_HOOK_EX(ShowCursor) (BOOL bShow)
{
	if (g_bBrowserShowing)
	{
		cursor_info.showing = !!bShow;
		return bShow ? 0 : -1;
	}

	return s_HookShowCursor.Call(bShow);
};

DECLARE_HOOK(SetPhysicalCursorPos, [](int X, int Y)
{
	return s_HookSetPhysicalCursorPos.Call(X, Y);
});

DECLARE_HOOK(GetPhysicalCursorPos, [](LPPOINT lpPoint)
{
	return s_HookGetPhysicalCursorPos.Call(lpPoint);
});

static RECT clip_cursor_rect;
static bool cursor_clipped = false;
static bool overlay_clip_saved = false;
DECLARE_HOOK(ClipCursor, [](CONST RECT *lpRect) -> BOOL
{
	cursor_clipped = !!lpRect;
	if (cursor_clipped)
		clip_cursor_rect = *lpRect;

	if (g_bBrowserShowing)
	{
		if (overlay_clip_saved)
			overlay_clip_saved = cursor_clipped;

		return true;
	}

	return s_HookClipCursor.Call(lpRect);
});

DECLARE_HOOK(GetClipCursor, [](LPRECT lpRect) -> BOOL
{
	if (!g_bBrowserShowing)
		return s_HookGetClipCursor.Call(lpRect);

	if (!lpRect)
		return true;

	if (cursor_clipped)
	{
		*lpRect = clip_cursor_rect;
		return true;
	}

	MONITORINFO info;
	info.cbSize = sizeof(info);
	GetMonitorInfoW(MonitorFromWindow(g_Proc.m_Stats.m_hWndCap, MONITOR_DEFAULTTOPRIMARY), &info);

	*lpRect = info.rcMonitor; // no idea if the coordinate systems match here

	return true;
});

static bool HandlePeekMessage(LPMSG lpMsg, UINT wRemoveMsg);
DECLARE_HOOK(PeekMessageA, [](LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) -> BOOL
{
	BOOL res = false;
	while ((res = s_HookPeekMessageA.Call(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg)))
		if (!HandlePeekMessage(lpMsg, wRemoveMsg))
			break;
	return res;
});

DECLARE_HOOK(PeekMessageW, [](LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) -> BOOL
{
	BOOL res = false;
	while ((res = s_HookPeekMessageW.Call(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg)))
		if (!HandlePeekMessage(lpMsg, wRemoveMsg))
			break;
	return res;
});

// GetCursorInfo seems to be buffered for DX9 somehow? doesn't seem to change state immediately; limiting the ShowCursor calls to 3 seems to be good enough for now
void OverlaySaveShowCursor()
{
	CURSORINFO info;
	info.cbSize = sizeof(CURSORINFO);
	if (!GetCursorInfo(&info))
		return;

	cursor_info.saved = true;
	cursor_info.showing = info.flags == CURSOR_SHOWING;

	if (!s_HookShowCursor.hook.IsHookInstalled())
		return;

	info.cbSize = sizeof(CURSORINFO);
	for (size_t i = 0; i < 3 && GetCursorInfo(&info) && !info.flags; i++)
	{
		if (s_HookShowCursor.Call(true) >= 0)
			break;

		info.cbSize = sizeof(CURSORINFO);
	}
}

void OverlayRestoreShowCursor()
{
	if (!cursor_info.saved || cursor_info.showing)
		return;

	cursor_info.saved = false;

	if (!s_HookShowCursor.hook.IsHookInstalled())
		return;

	CURSORINFO info;
	info.cbSize = sizeof(CURSORINFO);
	for (size_t i = 0; i < 3 && GetCursorInfo(&info) && info.flags == CURSOR_SHOWING; i++)
	{
		if (s_HookShowCursor.Call(false) < 0)
			break;

		info.cbSize = sizeof(CURSORINFO);
	}
}

void OverlayUnclipCursor()
{
	if (!s_HookGetClipCursor.hook.IsHookInstalled())
		return;

	if (s_HookGetClipCursor.Call(&clip_cursor_rect))
		overlay_clip_saved = true;

	s_HookClipCursor.Call(nullptr);
}

void OverlayRestoreClipCursor()
{
	if (!s_HookGetClipCursor.hook.IsHookInstalled())
		return;

	if (!overlay_clip_saved)
		return;

	overlay_clip_saved = false;
	s_HookClipCursor.Call(&clip_cursor_rect);
}

#ifdef USE_DIRECTI

bool GetDIHookOffsets( HINSTANCE hInst )
{
	if ( s_nDI8_GetDeviceState || s_nDI8_GetDeviceData )
		return true;

	CDllFile dll;
	HRESULT hRes = dll.FindDll( L"dinput8.dll" );
	if (IS_ERROR(hRes))
	{
		LOG_MSG( "GetDIHookOffsets: Failed to load dinput8.dll (0x%08x)" LOG_CR, hRes );
		return false;
	}

	s_DirectInput8Create = (DIRECTINPUT8CREATE)dll.GetProcAddress( "DirectInput8Create" );
	if (!s_DirectInput8Create) 
	{
		HRESULT hRes = HRes_GetLastErrorDef( HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED) );
		LOG_MSG( "GetDIHookOffsets: lookup for DirectInput8Create failed. (0x%08x)" LOG_CR, hRes );
		return false;
	}

	IRefPtr<IDirectInput8> pDI;
	IRefPtr<IDirectInputDevice8> pDevice;

	hRes = s_DirectInput8Create( hInst, DIRECTINPUT_VERSION, IID_IDirectInput8, IREF_GETPPTR(pDI, IDirectInput8), NULL);
	if ( FAILED(hRes) )
	{
		// DirectInput not available; take appropriate action 
		LOG_MSG( "GetDIHookOffsets: DirectInput8Create failed. 0x%08x" LOG_CR, hRes );
		return false;
	}

	hRes = pDI->CreateDevice(GUID_SysKeyboard, IREF_GETPPTR(pDevice, IDirectInputDevice8), NULL);
	if ( FAILED(hRes) )
	{
		LOG_MSG( "GetDIHookOffsets: IDirectInput8::CreateDevice() FAILED. 0x%08x" LOG_CR, hRes );
		return false;
	}

	UINT_PTR* pVTable = (UINT_PTR*)(*((UINT_PTR*)pDevice.get_RefObj()));
	s_nDI8_GetDeviceState = ( pVTable[DI8_DEVICE_GetDeviceState] - dll.get_DllInt());
	//LOG_MSG( "GetDIHookOffsets: dll base 0x%08x GetDeviceState offset 0x%08x\n", dll.get_DllInt( ), s_nDI8_GetDeviceState );
	s_nDI8_GetDeviceData = ( pVTable[DI8_DEVICE_GetDeviceData] - dll.get_DllInt());
	//LOG_MSG( "GetDIHookOffsets: dll base 0x%08x GetDeviceData offset 0x%08x\n", dll.get_DllInt( ), s_nDI8_GetDeviceData );	

	return true;
}

HRESULT WINAPI DI8_GetDeviceState( IDirectInputDevice8 *pDevice, DWORD dwSize, LPVOID lpState )
{
	s_HookDeviceState.SwapOld( s_DI8_GetDeviceState );

	// do our preliminary shit here - store device pointer, etc
	//LOG_MSG( "DI8_GetDeviceState: called for device 0x%08x"LOG_CR, pDevice );

	HRESULT hRes = s_HookDeviceState.Call(s_DI8_GetDeviceState, pDevice, dwSize, lpState );

	// do our shit here, eyeballing and messing with the input

	s_HookDeviceState.SwapReset( s_DI8_GetDeviceState );
	
	return hRes;
}

HRESULT WINAPI DI8_GetDeviceData( IDirectInputDevice8 *pDevice, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags )
{
	s_HookDeviceData.SwapOld( s_DI8_GetDeviceState );

	// do our preliminary shit here - store device pointer, etc
	//LOG_MSG( "DI8_GetDeviceData: called for device 0x%08x"LOG_CR, pDevice );

	HRESULT hRes = s_HookDeviceData.Call(s_DI8_GetDeviceData, pDevice, cbObjectData, rgdod, pdwInOut, dwFlags );

	// do our shit here, eyeballing and messing with the input

	s_HookDeviceData.SwapReset( s_DI8_GetDeviceState );

	return hRes;
}

bool HookDI( UINT_PTR dll_base )
{
	if ( !s_nDI8_GetDeviceState || !s_nDI8_GetDeviceData )
		return true;

	LOG_MSG( "DI8:HookFunctions: dinput8.dll loaded at 0x%08x" LOG_CR, dll_base );
	s_DI8_GetDeviceState = (GETDEVICESTATE)(dll_base + s_nDI8_GetDeviceState);
	if ( !s_HookDeviceState.InstallHook(s_DI8_GetDeviceState, DI8_GetDeviceState) )
	{
		LOG_MSG( "DI8:HookFunctions: unable to hook DirectInput function GetDeviceState" LOG_CR );
		//return false;
	}

	s_DI8_GetDeviceData = (GETDEVICEDATA)(dll_base + s_nDI8_GetDeviceData);
	if ( !s_HookDeviceData.InstallHook(s_DI8_GetDeviceData, DI8_GetDeviceData) )
	{
		LOG_MSG( "DI8:HookFunctions: unable to hook DirectInput function GetDeviceData" LOG_CR );
		//return false;
	}

	return true;
}

void UnhookDI( void )
{
	s_HookDeviceState.RemoveHook( s_DI8_GetDeviceState );
	s_HookDeviceData.RemoveHook( s_DI8_GetDeviceData );
}
#endif

BOOL WINAPI Hook_GetKeyboardState( PBYTE lpKeyState )
{
	s_HookGetKeyboardState.SwapOld( s_GetKeyboardState );
	//LOG_MSG( "Hook_GetKeyboardState: called"LOG_CR );
	BOOL res = s_HookGetKeyboardState.Call(s_GetKeyboardState, lpKeyState );
	// update our saved key states to generate events. can also modify the provided state if we're showing overlay (to hide input from the game)
	UpdateKeyboardState( lpKeyState );
	s_HookGetKeyboardState.SwapReset( s_GetKeyboardState );
	return res;
}

SHORT WINAPI Hook_GetAsyncKeyState( int vKey )
{
	s_HookGetAsyncKeyState.SwapOld( s_GetAsyncKeyState );
	//LOG_MSG( "Hook_GetAsyncKeyState: called for key %u"LOG_CR, vKey );
	SHORT res = s_HookGetAsyncKeyState.Call(s_GetAsyncKeyState, vKey );
	res = UpdateSingleKeyState( vKey, res );
	// mess with input here if we're in overlay mode
	s_HookGetAsyncKeyState.SwapReset( s_GetAsyncKeyState );
	return res;
}

static POINT saved_mouse_pos;
static bool mouse_pos_saved = false;

BOOL WINAPI Hook_GetCursorPos( LPPOINT lpPoint )
{
	s_HookGetCursorPos.SwapOld( s_GetCursorPos );
	BOOL res = s_HookGetCursorPos.Call(s_GetCursorPos, lpPoint );

	if (g_bBrowserShowing)
	{
		if (!mouse_pos_saved && lpPoint)
			saved_mouse_pos = *lpPoint;

		if (mouse_pos_saved && lpPoint)
			*lpPoint = saved_mouse_pos;
	}
	else
	{
		if (mouse_pos_saved)
		{
			SetCursorPos(saved_mouse_pos.x, saved_mouse_pos.y);
			if (lpPoint)
				*lpPoint = saved_mouse_pos;
		}
		mouse_pos_saved = false;
	}

	// mess with it here
	//LOG_MSG( "Hook_GetCursorPos: current pos is [%u, %u]"LOG_CR, lpPoint->x, lpPoint->y );
	s_HookGetCursorPos.SwapReset( s_GetCursorPos );
	return res;
}

BOOL WINAPI Hook_SetCursorPos( INT x, INT y )
{
	s_HookSetCursorPos.SwapOld( s_SetCursorPos );

	BOOL res = true;
	if (g_bBrowserShowing)
	{
		saved_mouse_pos.x = x;
		saved_mouse_pos.y = y;
		mouse_pos_saved = true;
	}
	else
		res = s_HookSetCursorPos.Call(s_SetCursorPos, x, y);

	// mess with it here
	//LOG_MSG( "Hook_SetCursorPos: setting pos to [%u, %u]"LOG_CR, x, y );
	s_HookSetCursorPos.SwapReset( s_SetCursorPos );
	return res;
}

void UpdateRawMouse(RAWMOUSE &event);
UINT WINAPI Hook_GetRawInputData( HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader )
{
	s_HookGetRawInputData.SwapOld( s_GetRawInputData );
	UINT res = s_HookGetRawInputData.Call(s_GetRawInputData, hRawInput, uiCommand, pData, pcbSize, cbSizeHeader );
	//LOG_MSG( "Hook_GetRawInputData: called with command %u"LOG_CR, uiCommand );
	if ( pData ) // called with pData == NULL means they're just asking for a size to allocate
	{
		RAWINPUT* raw = (RAWINPUT*)pData;
		if ( raw->header.dwType == RIM_TYPEKEYBOARD )
			UpdateRawKeyState( &(raw->data.keyboard) );
		else if (raw->header.dwType == RIM_TYPEMOUSE)
			UpdateRawMouse(raw->data.mouse);
	}
	s_HookGetRawInputData.SwapReset( s_GetRawInputData );
	return res;
}

UINT WINAPI Hook_GetRawInputBuffer( PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader )
{
	s_HookGetRawInputBuffer.SwapOld( s_GetRawInputBuffer );
	UINT res = s_HookGetRawInputBuffer.Call(s_GetRawInputBuffer, pData, pcbSize, cbSizeHeader );
	//LOG_MSG( "Hook_GetRawInputBuffer: called"LOG_CR );
	s_HookGetRawInputBuffer.SwapReset( s_GetRawInputBuffer );
	return res;
}

static std::vector<RAWINPUTDEVICE> prev_devices;

DECLARE_HOOK(GetRegisteredRawInputDevices, [](PRAWINPUTDEVICE pRawInputDevices, PUINT puiNumDevices, UINT cbSize) -> UINT
{
	if (g_bBrowserShowing)
	{
		if (!pRawInputDevices || *puiNumDevices < prev_devices.size())
		{
			*puiNumDevices = prev_devices.size();
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}

		for (size_t i = 0, end_ = prev_devices.size(); i < end_; i++)
			pRawInputDevices[i] = prev_devices[i];

		return prev_devices.size();
	}

	return s_HookGetRegisteredRawInputDevices.Call(pRawInputDevices, puiNumDevices, cbSize);
});

DECLARE_HOOK(RegisterRawInputDevices, [](PCRAWINPUTDEVICE pRawInputDevices, UINT uiNumDevices, UINT cbSize) -> BOOL
{
	if (g_bBrowserShowing)
	{
		prev_devices.erase(std::remove_if(begin(prev_devices), end(prev_devices), [&](const auto &dev)
		{
			for (auto i = 0u; i < uiNumDevices; i++)
			{
				auto &reg_dev = pRawInputDevices[i];
				if (reg_dev.usUsage != dev.usUsage || reg_dev.usUsagePage != dev.usUsagePage)
					continue;

				if ((reg_dev.dwFlags & RIDEV_REMOVE) && !reg_dev.hwndTarget)
					return true;
			}

			return false;
		}), end(prev_devices));

		for (auto i = 0u; i < uiNumDevices; i++)
		{
			auto &reg_dev = pRawInputDevices[i];
			if (reg_dev.dwFlags & RIDEV_REMOVE)
				continue;

			prev_devices.push_back(reg_dev);
		}
		return true;
	}

	return s_HookRegisterRawInputDevices.Call(pRawInputDevices, uiNumDevices, cbSize);
});

extern ProtectedObject<HCURSOR> overlay_cursor;
static HCURSOR old_cursor = nullptr;

DECLARE_HOOK_EX(SetCursor) (HCURSOR hCursor)
{
	if (g_bBrowserShowing)
	{
		auto res = old_cursor;
		old_cursor = hCursor;
		return res;
	}

	return s_HookSetCursor.Call(hCursor);
};

void ShowOverlayCursor()
{
	old_cursor = s_HookSetCursor.Call(*overlay_cursor.Lock());

	CURSORINFO info;
	info.cbSize = sizeof(CURSORINFO);
	for (size_t i = 0; i < 3 && GetCursorInfo(&info) && !info.flags; i++)
	{
		if (s_HookShowCursor.Call(true) >= 0)
			break;

		info.cbSize = sizeof(CURSORINFO);
	}
}

void RestoreCursor()
{
	OverlayRestoreClipCursor();
	OverlayRestoreShowCursor();
	s_HookSetCursor.Call(old_cursor);
}

void ResetOverlayCursor()
{
	if (!g_bBrowserShowing)
		return;

	s_HookSetCursor.Call(*overlay_cursor.Lock());
}

HCURSOR WINAPI Hook_GetCursor(VOID)
{
	if (g_bBrowserShowing)
		return old_cursor;

	s_HookGetCursor.SwapOld(s_GetCursor);
	auto res = s_HookGetCursor.Call(s_GetCursor);
	s_HookGetCursor.SwapReset(s_GetCursor);
	return res;
}

static bool UpdateCursorState()
{
	static bool was_showing = false;

	if (was_showing != g_bBrowserShowing)
	{
		if (g_bBrowserShowing)
			ShowOverlayCursor();
		else
			RestoreCursor();

		was_showing = g_bBrowserShowing;

		return true;
	}

	if (!g_bBrowserShowing)
		return false;

	ResetOverlayCursor();
	return true;
}

template <typename T>
static bool InitHook(HMODULE dll, T *&orig, T* new_, const char *name, CHookJump &hook)
{
	orig = (T*)GetProcAddress(dll, name);
	if (hook.InstallHook(orig, new_))
		return true;

	hlog("HookInput: unable to hook function %s", name);
	return false;
}

template <typename T>
static bool InitHook(HMODULE dll, T &hook)
{
	auto orig = (decltype(hook.original))GetProcAddress(dll, hook.func_name);
	if (hook.Install(orig))
		return true;

	hlog("HookInput: unable to hook function %s", hook.func_name);
	return false;
}

static std::once_flag init_mh;
bool CHookJump::installed = false;

static bool InitHooks()
{
	static bool hooks_ready = false;
	std::call_once(init_mh, []
	{
		if ([]
		{
			if (MH_Initialize())
			{
				hlog("MH_Initialize failed");
				return false;
			}

			HMODULE dll = GetModuleHandle(L"user32.dll");
			s_GetKeyboardState = (GETKEYBOARDSTATE)GetProcAddress(dll, "GetKeyboardState");
			if (!s_HookGetKeyboardState.InstallHook(s_GetKeyboardState, Hook_GetKeyboardState))
			{
				LOG_MSG("HookInput: unable to hook function GetKeyboardState" LOG_CR);
				return false;
			}

			s_GetAsyncKeyState = (GETASYNCKEYSTATE)GetProcAddress(dll, "GetAsyncKeyState");
			if (!s_HookGetAsyncKeyState.InstallHook(s_GetAsyncKeyState, Hook_GetAsyncKeyState))
			{
				LOG_MSG("HookInput: unable to hook function GetAsyncKeyState" LOG_CR);
				return false;
			}

			s_GetCursorPos = (GETCURSORPOS)GetProcAddress(dll, "GetCursorPos");
			if (!s_HookGetCursorPos.InstallHook(s_GetCursorPos, Hook_GetCursorPos))
			{
				LOG_MSG("HookInput: unable to hook function GetCursorPos" LOG_CR);
				return false;
			}

			s_SetCursorPos = (SETCURSORPOS)GetProcAddress(dll, "SetCursorPos");
			if (!s_HookSetCursorPos.InstallHook(s_SetCursorPos, Hook_SetCursorPos))
			{
				LOG_MSG("HookInput: unable to hook function SetCursorPos" LOG_CR);
				return false;
			}

			s_GetRawInputData = (GETRAWINPUTDATA)GetProcAddress(dll, "GetRawInputData");
			if (!s_HookGetRawInputData.InstallHook(s_GetRawInputData, Hook_GetRawInputData))
			{
				LOG_MSG("HookInput: unable to hook function GetRawInputData" LOG_CR);
				return false;
			}

			s_GetRawInputBuffer = (GETRAWINPUTBUFFER)GetProcAddress(dll, "GetRawInputBuffer");
			if (!s_HookGetRawInputBuffer.InstallHook(s_GetRawInputBuffer, Hook_GetRawInputBuffer))
			{
				LOG_MSG("HookInput: unable to hook function GetRawInputBuffer" LOG_CR);
				return false;
			}

#ifdef HOOK_REGISTER_RAW_DEVICES
			if (!InitHook(dll, s_HookGetRegisteredRawInputDevices))
				return false;

			if (!InitHook(dll, s_HookRegisterRawInputDevices))
				return false;
#endif

			if (!InitHook(dll, s_HookSetCursor))
				return false;

			if (!InitHook(dll, s_GetCursor, Hook_GetCursor, "GetCursor", s_HookGetCursor))
				return false;

			if (!InitHook(dll, s_HookShowCursor))
				return false;

			if (!InitHook(dll, s_HookSetPhysicalCursorPos))
				return false;

			if (!InitHook(dll, s_HookGetPhysicalCursorPos))
				hlog("GetPhysicalCursorPos not available (probably the same as GetCursorPos, on windows 8.1+?)");

			InitHook(dll, s_HookClipCursor);
			InitHook(dll, s_HookGetClipCursor);

			InitHook(dll, s_HookPeekMessageA);
			InitHook(dll, s_HookPeekMessageW);

			return true;
		}())
			hooks_ready = true;
	});

	return hooks_ready;
}

bool HookInput( void )
{
	if (!InitHooks())
		return false;

	if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
	{
		hlog("MH_EnableHook failed");
		return false;
	}

	CHookJump::installed = true;

	return true;
}

void UnhookInput( void )
{
	if (!InitHooks())
		return;

	CHookJump::installed = false;

	MH_DisableHook(MH_ALL_HOOKS);
}

// handle any input events sent to game's window. return true if we're eating them (ie: showing overlay)
// we should try to keep this code simple and pass messages off to appropriate handler functions.
bool InputWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LPMSG lpMsg=nullptr)
{
	auto handleKey = [&](KeyEventType type)
	{
		auto res = UpdateWMKeyState(wParam, type);
		if (g_bBrowserShowing)
			ForgeEvent::KeyEvent(uMsg, wParam, lParam);
		if (g_bBrowserShowing && uMsg != WM_CHAR && lpMsg)
			TranslateMessage(lpMsg);

		return res;
	};

	switch ( uMsg )
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
			return UpdateMouseState(uMsg, wParam, lParam);
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			return handleKey(KEY_DOWN);
		case WM_KEYUP:
		case WM_SYSKEYUP:
			return handleKey(KEY_UP);
		case WM_CHAR:
			return handleKey(KEY_CHAR);
		case WM_SETCURSOR:
			return UpdateCursorState();

#ifdef HOOK_REGISTER_RAW_DEVICES
		case WM_INPUT:
			if (g_bBrowserShowing)
				return DefWindowProc(hWnd, uMsg, wParam, lParam);
			return false;
#endif
	}

	return false;
}


static bool HandlePeekMessage(LPMSG lpMsg, UINT wRemoveMsg)
{
	if (!(wRemoveMsg & PM_REMOVE) || !lpMsg) // The Witness seems to always use PM_REMOVE, not sure what to do about games that use PM_NOREMOVE and actually do stuff with the message
		return false;

	if (InputWndProc(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam, lpMsg))
		return true;

	return false;
}

static WNDPROC prev_proc = nullptr;
LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// call input handler
	if (InputWndProc(hWnd, uMsg, wParam, lParam))
		return 0;

	return ::CallWindowProc(prev_proc, hWnd, uMsg, wParam, lParam);
}

void HookWndProc()
{
	WNDPROC current = (WNDPROC)GetWindowLongPtr(g_Proc.m_Stats.m_hWndCap, GWLP_WNDPROC);
	// save the current handler if we don't know about it yet
	if (!prev_proc)
		prev_proc = current;

	// don't mess with shit unless it's the original wndproc. we don't want to get mixed up with anything else hooking it or end up recursively calling ourselves
	if (current == HookedWndProc || current != prev_proc)
		return;

	prev_proc = (WNDPROC)SetWindowLongPtr(g_Proc.m_Stats.m_hWndCap, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
	if (!prev_proc)
		LOG_MSG("HookWndProc: SetWindowLongPtr failed, wndproc hook disabled"LOG_CR);
}

void DisableRawInput()
{
#ifdef HOOK_REGISTER_RAW_DEVICES
	UINT num = prev_devices.size();
	if (s_HookGetRegisteredRawInputDevices.Call(prev_devices.data(), &num, sizeof(RAWINPUTDEVICE)) != num || num != prev_devices.size())
	{
		prev_devices.resize(num);
		s_HookGetRegisteredRawInputDevices.Call(prev_devices.data(), &num, sizeof(RAWINPUTDEVICE));
	}

	if (!prev_devices.size())
		return;

	auto devices = prev_devices;
	for (auto &dev : devices)
	{
		dev.hwndTarget = nullptr;
		dev.dwFlags = RIDEV_REMOVE;
	}

	s_HookRegisterRawInputDevices.Call(devices.data(), devices.size(), sizeof(RAWINPUTDEVICE));
#endif
}

void RestoreRawInput()
{
#ifdef HOOK_REGISTER_RAW_DEVICES
	if (!prev_devices.size())
		return;

	s_HookRegisterRawInputDevices.Call(prev_devices.data(), prev_devices.size(), sizeof(RAWINPUTDEVICE));
	prev_devices.resize(0);
#endif
}
