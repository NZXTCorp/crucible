// [InputHooks.cpp 2014-03-12 abright]
// new home for input hooking code

#include "stdafx.h"

#include "TaksiInput.h"

#include "InputHooks.h"
#include "KeyboardInput.h"

#include <vector>

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

static CHookJump s_HookGetKeyboardState;
static CHookJump s_HookGetAsyncKeyState;
static CHookJump s_HookGetCursorPos;
static CHookJump s_HookSetCursorPos;
static CHookJump s_HookGetRawInputData;
static CHookJump s_HookGetRawInputBuffer;
static CHookJump s_HookGetRegisteredRawInputDevices;
static CHookJump s_HookRegisterRawInputDevices;

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

	HRESULT hRes = s_DI8_GetDeviceState( pDevice, dwSize, lpState );

	// do our shit here, eyeballing and messing with the input

	s_HookDeviceState.SwapReset( s_DI8_GetDeviceState );
	
	return hRes;
}

HRESULT WINAPI DI8_GetDeviceData( IDirectInputDevice8 *pDevice, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags )
{
	s_HookDeviceData.SwapOld( s_DI8_GetDeviceState );

	// do our preliminary shit here - store device pointer, etc
	//LOG_MSG( "DI8_GetDeviceData: called for device 0x%08x"LOG_CR, pDevice );

	HRESULT hRes = s_DI8_GetDeviceData( pDevice, cbObjectData, rgdod, pdwInOut, dwFlags );

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
	BOOL res = s_GetKeyboardState( lpKeyState );
	// update our saved key states to generate events. can also modify the provided state if we're showing overlay (to hide input from the game)
	UpdateKeyboardState( lpKeyState );
	s_HookGetKeyboardState.SwapReset( s_GetKeyboardState );
	return res;
}

SHORT WINAPI Hook_GetAsyncKeyState( int vKey )
{
	s_HookGetAsyncKeyState.SwapOld( s_GetAsyncKeyState );
	//LOG_MSG( "Hook_GetAsyncKeyState: called for key %u"LOG_CR, vKey );
	SHORT res = s_GetAsyncKeyState( vKey );
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
	BOOL res = s_GetCursorPos( lpPoint );

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
		res = s_SetCursorPos(x, y);

	// mess with it here
	//LOG_MSG( "Hook_SetCursorPos: setting pos to [%u, %u]"LOG_CR, x, y );
	s_HookSetCursorPos.SwapReset( s_SetCursorPos );
	return res;
}

void UpdateRawMouse(RAWMOUSE &event);
UINT WINAPI Hook_GetRawInputData( HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader )
{
	s_HookGetRawInputData.SwapOld( s_GetRawInputData );
	UINT res = s_GetRawInputData( hRawInput, uiCommand, pData, pcbSize, cbSizeHeader );
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
	UINT res = s_GetRawInputBuffer( pData, pcbSize, cbSizeHeader );
	//LOG_MSG( "Hook_GetRawInputBuffer: called"LOG_CR );
	s_HookGetRawInputBuffer.SwapReset( s_GetRawInputBuffer );
	return res;
}

static std::vector<RAWINPUTDEVICE> prev_devices;
static bool devices_saved = false;

UINT WINAPI Hook_GetRegisteredRawInputDevices(PRAWINPUTDEVICE pRawInputDevices, PUINT puiNumDevices, UINT cbSize)
{
	if (g_bBrowserShowing)
	{
		if (!devices_saved)
		{
			s_HookGetRegisteredRawInputDevices.SwapOld(s_GetRegisteredRawInputDevices);
			prev_devices.resize(0);
			UINT num = 0;
			auto res = s_GetRegisteredRawInputDevices(nullptr, &num, sizeof(RAWINPUTDEVICE));
			if (res < 0)
			{
				prev_devices.resize(num);
				res = s_GetRegisteredRawInputDevices(prev_devices.data(), &num, sizeof(RAWINPUTDEVICE));
				if (res < 0)
					goto orig;
				prev_devices.resize(res);

				hlog("Saved %d (%d) devices", res, num);
			}
			devices_saved = true;
			s_HookGetRegisteredRawInputDevices.SwapReset(s_GetRegisteredRawInputDevices);
		}

		if (!pRawInputDevices || *puiNumDevices < prev_devices.size())
		{
			*puiNumDevices = prev_devices.size();
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return -1;
		}

		for (size_t i = 0, end_ = prev_devices.size(); i < end_; i++)
			pRawInputDevices[i] = prev_devices[i];

		return prev_devices.size();
	}
	else if (devices_saved)
	{
		RegisterRawInputDevices(prev_devices.data(), prev_devices.size(), sizeof(RAWINPUTDEVICE));
		prev_devices.resize(0);
		devices_saved = false;
	}

	s_HookGetRegisteredRawInputDevices.SwapOld(s_GetRegisteredRawInputDevices);
orig:
	auto res = s_GetRegisteredRawInputDevices(pRawInputDevices, puiNumDevices, cbSize);
	s_HookGetRegisteredRawInputDevices.SwapReset(s_GetRegisteredRawInputDevices);
	return res;
}

BOOL WINAPI Hook_RegisterRawInputDevices(PCRAWINPUTDEVICE pRawInputDevices, UINT uiNumDevices, UINT cbSize)
{
	s_HookRegisterRawInputDevices.SwapOld(s_RegisterRawInputDevices);
	auto res = s_RegisterRawInputDevices(pRawInputDevices, uiNumDevices, cbSize);
	s_HookRegisterRawInputDevices.SwapReset(s_RegisterRawInputDevices);
	return res;
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

bool HookInput( void )
{
	HMODULE dll = GetModuleHandle( L"user32.dll" );
	s_GetKeyboardState = (GETKEYBOARDSTATE)GetProcAddress( dll, "GetKeyboardState" );
	if ( !s_HookGetKeyboardState.InstallHook( s_GetKeyboardState, Hook_GetKeyboardState ) )
	{
		LOG_MSG( "HookInput: unable to hook function GetKeyboardState" LOG_CR );
		return false;
	}

	s_GetAsyncKeyState = (GETASYNCKEYSTATE)GetProcAddress( dll, "GetAsyncKeyState" );
	if ( !s_HookGetAsyncKeyState.InstallHook( s_GetAsyncKeyState, Hook_GetAsyncKeyState ) )
	{
		LOG_MSG( "HookInput: unable to hook function GetAsyncKeyState" LOG_CR );
		return false;
	}

	s_GetCursorPos = (GETCURSORPOS)GetProcAddress( dll, "GetCursorPos" );
	if ( !s_HookGetCursorPos.InstallHook( s_GetCursorPos, Hook_GetCursorPos ) )
	{
		LOG_MSG( "HookInput: unable to hook function GetCursorPos" LOG_CR );
		return false;
	}

	s_SetCursorPos = (SETCURSORPOS)GetProcAddress( dll, "SetCursorPos" );
	if ( !s_HookSetCursorPos.InstallHook( s_SetCursorPos, Hook_SetCursorPos ) )
	{
		LOG_MSG( "HookInput: unable to hook function SetCursorPos" LOG_CR );
		return false;
	}

	s_GetRawInputData = (GETRAWINPUTDATA)GetProcAddress( dll, "GetRawInputData" );
	if ( !s_HookGetRawInputData.InstallHook( s_GetRawInputData, Hook_GetRawInputData ) )
	{
		LOG_MSG( "HookInput: unable to hook function GetRawInputData" LOG_CR );
		return false;
	}

	s_GetRawInputBuffer = (GETRAWINPUTBUFFER)GetProcAddress( dll, "GetRawInputBuffer" );
	if ( !s_HookGetRawInputBuffer.InstallHook( s_GetRawInputBuffer, Hook_GetRawInputBuffer ) )
	{
		LOG_MSG( "HookInput: unable to hook function GetRawInputBuffer" LOG_CR );
		return false;
	}

	if (!InitHook(dll, s_GetRegisteredRawInputDevices, Hook_GetRegisteredRawInputDevices, "GetRegisteredRawInputDevices", s_HookGetRegisteredRawInputDevices))
		return false;

	if (!InitHook(dll, s_RegisterRawInputDevices, Hook_RegisterRawInputDevices, "RegisterRawInputDevices", s_HookRegisterRawInputDevices))
		return false;

	return true;
}

void UnhookInput( void )
{
	s_HookGetKeyboardState.RemoveHook( s_GetKeyboardState );
	s_HookGetAsyncKeyState.RemoveHook( s_GetAsyncKeyState );
	s_HookGetCursorPos.RemoveHook( s_GetCursorPos );
	s_HookSetCursorPos.RemoveHook( s_SetCursorPos );
	s_HookGetRawInputData.RemoveHook( s_GetRawInputData );
	s_HookGetRawInputBuffer.RemoveHook( s_GetRawInputBuffer );
	s_HookGetRegisteredRawInputDevices.RemoveHook(s_GetRegisteredRawInputDevices);
	s_HookRegisterRawInputDevices.RemoveHook(s_RegisterRawInputDevices);
}

// handle any input events sent to game's window. return true if we're eating them (ie: showing overlay)
// we should try to keep this code simple and pass messages off to appropriate handler functions.
bool InputWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	switch ( uMsg )
	{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			return UpdateWMKeyState( wParam, KEY_DOWN );
		case WM_KEYUP:
		case WM_SYSKEYUP:
			return UpdateWMKeyState( wParam, KEY_UP );
		case WM_CHAR:
			return UpdateWMKeyState( wParam, KEY_CHAR );
	}

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
	UINT num = 0;
	s_GetRegisteredRawInputDevices(nullptr, &num, sizeof(RAWINPUTDEVICE));

	RegisterRawInputDevices(nullptr, 0, sizeof(RAWINPUTDEVICE));
}

void RestoreRawInput()
{
	UINT num = 0;
	s_GetRegisteredRawInputDevices(nullptr, &num, sizeof(RAWINPUTDEVICE));
}
