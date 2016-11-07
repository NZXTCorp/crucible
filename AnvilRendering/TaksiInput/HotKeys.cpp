//
// HotKeys.cpp
//
#include "stdafx.h"
#include "AnvilRendering.h"
#include "GAPI_render/IRefPtr.h"
#include "HotKeys.h"
#include <commctrl.h>	// HOTKEYF_ALT

#include "InputHooks.h" // input hooking
#include "KeyboardInput.h" // input capture

CTaksiHotKeys g_HotKeys;		// what does the user want to do?
CTaksiKeyboard g_UserKeyboard;		// keyboard hook handle. if i cant hook DI. Just for this process.

//#define USE_KEYBOARD_HOOK

#ifdef USE_DIRECTI
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

CTaksiDI g_UserDI;	// DirectInput wrapper.

typedef HRESULT (WINAPI *DIRECTINPUT8CREATE)(HINSTANCE, DWORD, REFIID, LPVOID, IDirectInput8**);
static DIRECTINPUT8CREATE s_DirectInput8Create = NULL;

static IRefPtr<IDirectInput8> s_lpDI;
static IRefPtr<IDirectInputDevice8> s_lpDIDevice;
#endif

#ifndef _T
#define _T(x) L ## x
#endif

//**************************************************************************************

#ifdef USE_DIRECTI

CTaksiDI::CTaksiDI()
	: m_bSetup(false)
{
	ZeroMemory(m_abHotKey,sizeof(m_abHotKey));
}

bool CTaksiDI::Hook( void )
{
#ifdef USE_INPUT_HOOKS
	if (!FindDll(_T("dinput8.dll")) )
	{
		LOG_MSG( "CTaksiDI:Hook: couldn't find dinput8.dll" LOG_CR );
		return false;
	}

	return HookDI( get_DllInt( ) );
#else
		return true;
#endif
}

void CTaksiDI::Unhook( void )
{
#ifdef USE_INPUT_HOOKS
	UnhookDI( );
#endif
}

bool CTaksiDI::IsHooked( void )
{
	if ( !s_lpDIDevice.IsValidRefObj( ) )
		return false;

	return true;
}

HRESULT CTaksiDI::SetupDirectInput()
{
	m_bSetup = false;
	if (!FindDll(_T("dinput8.dll"))) 
	{
		LOG_MSG( "SetupDirectInput: dinput8.dll not found" LOG_CR );
		/*
		HRESULT hRes = LoadDll(_T("dinput8.dll"));
		if ( IS_ERROR(hRes))
		{
			return hRes;
		}
		*/
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}

	DEBUG_MSG("SetupDirectInput: using DirectInput." LOG_CR );

	s_DirectInput8Create = (DIRECTINPUT8CREATE)GetProcAddress("DirectInput8Create");
	if (!s_DirectInput8Create) 
	{
		HRESULT hRes = HRes_GetLastErrorDef( HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED));
		LOG_MSG( "SetupDirectInput: lookup for DirectInput8Create failed. (0x%x)" LOG_CR, hRes );
		return hRes;
	}

	//HookDI( get_DllInt( ) );

	// why is this translation needed? this will only reliably translate into
	// scan codes for the left keys. why not just use the DIK_ defines in ProcessDirectInut()?
	static const BYTE sm_vKeysExt[ COUNTOF(m_bScanExt) ] = 
	{
		VK_SHIFT,		// HOTKEYF_SHIFT
		VK_CONTROL,		// HOTKEYF_CONTROL
		VK_MENU,		// HOTKEYF_ALT
	};
	for ( int i=0; i<COUNTOF(m_bScanExt); i++ )
	{
		m_bScanExt[i] = ::MapVirtualKey( sm_vKeysExt[i], 0);
	}

	m_bSetup = true;
	DEBUG_MSG("SetupDirectInput: done." LOG_CR );
	return S_OK;
}

void CTaksiDI::ProcessDirectInput()
{
	// process keyboard input using DirectInput
	// called inside PresentFrameBegin() ONLY
	if ( !IsValidDll( ) )	// need to call SetupDirectInput()
		return;
	ASSERT( m_bSetup );
	ASSERT( s_DirectInput8Create );

	HRESULT hRes;
	if ( !s_lpDIDevice.IsValidRefObj( ) )
	{
		// Create device
		hRes = s_DirectInput8Create( g_hInst, DIRECTINPUT_VERSION, IID_IDirectInput8, IREF_GETPPTR(s_lpDI, IDirectInput8), NULL );
		if ( FAILED(hRes) )
		{
			// DirectInput not available; take appropriate action 
			LOG_MSG( "ProcessDirectInput: DirectInput8Create failed. 0x%x" LOG_CR, hRes );
			return;
		}
		if (!s_lpDI.IsValidRefObj())
		{
			return;
		}
		hRes = s_lpDI->CreateDevice( GUID_SysKeyboard, IREF_GETPPTR(s_lpDIDevice, IDirectInputDevice8), NULL );
		if ( FAILED(hRes) )
		{
			LOG_MSG( "ProcessDirectInput: pDI->CreateDevice() failed. 0x%x" LOG_CR, hRes );
		}
		if ( !s_lpDIDevice.IsValidRefObj( ) )
		{
			return;
		}
		hRes = s_lpDIDevice->SetDataFormat( &c_dfDIKeyboard );
		if ( FAILED(hRes) )
		{
			LOG_MSG( "ProcessDirectInput: pDevice->SetDataFormat() failed. 0x%x" LOG_CR, hRes );
			return;
		} 
		
	}

	// for each frame: acquire, get state, unacquire
	//LOG_MSG( "ProcessDirectInput: checking keys" LOG_CR );

	hRes = s_lpDIDevice->Acquire( );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "ProcessDirectInput: pDevice->Acquire() failed. 0x%x" LOG_CR, hRes );
		hRes = s_lpDIDevice->Unacquire( );
		return;
	}

	char buffer[256]; 
	hRes = s_lpDIDevice->GetDeviceState( sizeof(buffer), (LPVOID)&buffer );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "ProcessDirectInput: pDevice->GetDeviceState() failed. 0x%x" LOG_CR, hRes );
		hRes = s_lpDIDevice->Unacquire( );
		return;
	}

	hRes = s_lpDIDevice->Unacquire( );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "ProcessDirectInput: pDevice->Unacquire() failed. 0x%x" LOG_CR, hRes );
		hRes = s_lpDIDevice->Unacquire( );
		return;
	}

	// Need to also handle the right set of keys.  MapVirtualKey() only really 
	// works for getting the left keys (despite what MSDN says).
	static const BYTE sm_diKeys[ COUNTOF(m_bScanExt) ] = 
	{
		DIK_RSHIFT,		// HOTKEYF_SHIFT
		DIK_RCONTROL,	// HOTKEYF_CONTROL
		DIK_RMENU		// HOTKEYF_ALT
	};
	// check for HOTKEYF_SHIFT, HOTKEYF_CONTROL, HOTKEYF_ALT
	BYTE bHotMask = 0;
	for ( int i=0; i<COUNTOF(m_bScanExt); i++ )
	{
		if ( (buffer[m_bScanExt[i]] & 0x80) || (buffer[sm_diKeys[i]] & 0x80) ) 
			bHotMask |= (1<<i);
	}

	for ( int i=0; i<HOTKEY_QTY; i++ )
	{
		WORD wHotKey = GetHotKey( (HOTKEY_TYPE)i );
		if (!wHotKey)
			continue;

		BYTE iScanCode = ::MapVirtualKey( LOBYTE(wHotKey), 0);
		if ( buffer[iScanCode] & 0x80 ) 
		{
			// key down.
			m_abHotKey[i] = true;
			continue;
		}
		// key up.
		if ( ! m_abHotKey[i] )
			continue;
		m_abHotKey[i] = false;
		if ( HIBYTE(wHotKey) != bHotMask )
			continue;

		// action on key up.
		g_HotKeys.ScheduleHotKey((HOTKEY_TYPE) i );
	}
}

void CTaksiDI::CloseDirectInput()
{
	if ( !IsValidDll( ) )
		return;

	//UnhookDI( );

	if ( s_lpDIDevice.IsValidRefObj( ) )
	{
		s_lpDIDevice->Unacquire( );
		s_lpDIDevice.ReleaseRefObj( );
	}
	s_lpDI.ReleaseRefObj( );
}

#endif

//**************************************************************** 

bool CTaksiKeyboard::InstallHookKeys( bool bDummy )
{
#ifndef USE_KEYBOARD_HOOK
	return true;
#endif
	// install keyboard hooks to all threads of this process.
	// ARGS:
	//  bDummy = a null hook just to keep us (this dll) loaded in process space.

	UninstallHookKeys( );
	LOG_MSG( "CTaksiKeyboard::InstallHookKeys(%d)." LOG_CR, bDummy );

	DWORD dwThreadId = ::GetWindowThreadProcessId( g_Proc.m_Stats.m_hWndCap, NULL );
	m_hHookKeys = ::SetWindowsHookEx( WH_KEYBOARD, ( bDummy ) ? DummyKeyboardProc : KeyboardProc, g_hInst, dwThreadId );

	for ( int i = 0; i < HOTKEY_QTY; i++ )
	{
		g_UserKeyboard.m_bKeysPressed[i] = false;
	}

	DEBUG_MSG( "CTaksiKeyboard::InstallHookKeys(%d)=%08x" LOG_CR, bDummy, (UINT_PTR)m_hHookKeys );
	return( m_hHookKeys != NULL );
}

void CTaksiKeyboard::UninstallHookKeys(void)
{
	// remove keyboard hooks 
	if (m_hHookKeys == NULL)
		return;

	::UnhookWindowsHookEx( m_hHookKeys );
	LOG_MSG( "CTaksiKeyboard::UninstallHookKeys 0x%x" LOG_CR, (UINT_PTR)m_hHookKeys );
	m_hHookKeys = NULL;
}

bool CTaksiKeyboard::IsHooked( void )
{
	return (m_hHookKeys != nullptr);
}

LRESULT CALLBACK CTaksiKeyboard::DummyKeyboardProc(int code, WPARAM wParam, LPARAM lParam) // static
{
	// do not process message. just pass thru
	return ::CallNextHookEx( g_UserKeyboard.m_hHookKeys, code, wParam, lParam ); 
}

LRESULT CALLBACK CTaksiKeyboard::KeyboardProc(int code, WPARAM wParam, LPARAM lParam) // static
{
	// SetWindowsHookEx WH_KEYBOARD
	// NOTE: NO idea what context this is called in!
	if (code==HC_ACTION)
	{
		bool eat_key = false;
		// NOT using DirectInpout.
		// process the key events 
		// a key was pressed/released. wParam = virtual key code
		if (lParam & (1<<31)) 
		{
			// a key release.
			DEBUG_MSG( "KeyboardProc HC_ACTION wParam=0%x" LOG_CR, wParam );
			BYTE bHotMask = g_UserKeyboard.m_bHotMask;
			if (lParam&(1<<29))
				bHotMask |= HOTKEYF_ALT;

			for (int i = 0; i < HOTKEY_QTY; i++ )
			{
				WORD wHotKey = GetHotKey( (HOTKEY_TYPE)i );
				if (!wHotKey)
					continue;

				if (wParam != LOBYTE(wHotKey))
					continue;
				if (bHotMask != HIBYTE(wHotKey))
					continue;
				g_UserKeyboard.m_bKeysPressed[i] = false; // clear the 'pressed' state
				if (!(eat_key = g_HotKeys.DoHotKey( (HOTKEY_TYPE)i, HKEVENT_RELEASE, wHotKey)))
					g_HotKeys.AddEvent( (HOTKEY_TYPE)i, HKEVENT_RELEASE );
			}

			switch ( wParam)
			{
				case VK_SHIFT:
					g_UserKeyboard.m_bHotMask &= ~HOTKEYF_SHIFT;
				break;
				case VK_CONTROL:
					g_UserKeyboard.m_bHotMask &= ~HOTKEYF_CONTROL;
				break;
			}
		}
		else
		{
			// a key press.
			switch ( wParam )
			{
				case VK_SHIFT:
					g_UserKeyboard.m_bHotMask |= HOTKEYF_SHIFT;
				break;
				
				case VK_CONTROL:
					g_UserKeyboard.m_bHotMask |= HOTKEYF_CONTROL;
				break;
			}

			// todo: remember if hotkeys are already 'pressed' so we can ignore subsequent press events (windows will spam them)
			BYTE bHotMask = g_UserKeyboard.m_bHotMask;
			if ( lParam & (1<<29) )
				bHotMask |= HOTKEYF_ALT;

			for (int i = 0; i < HOTKEY_QTY; i++ )
			{
				WORD wHotKey = GetHotKey( (HOTKEY_TYPE)i );
				if (!wHotKey)
					continue;

				if (wParam != LOBYTE(wHotKey))
					continue;

				if (bHotMask != HIBYTE(wHotKey))
					continue;

				if ( !g_UserKeyboard.m_bKeysPressed[i] )
				{
					g_UserKeyboard.m_bKeysPressed[i] = true;
					if (!(eat_key = g_HotKeys.DoHotKey((HOTKEY_TYPE)i, HKEVENT_PRESS, wHotKey)))
						g_HotKeys.AddEvent( (HOTKEY_TYPE)i, HKEVENT_PRESS );
				}
			}
		}
		if (g_bBrowserShowing || eat_key)
			return 1;
	}

	// We must pass the all messages on to CallNextHookEx.
	return ::CallNextHookEx(g_UserKeyboard.m_hHookKeys, code, wParam, lParam);
}

//********************************************************

static bool hotkeys_pressed[HOTKEY_QTY] = { false };

bool CTaksiHotKeys::DoHotKey( HOTKEY_TYPE eHotKey, HOTKEY_EVENT evt, WORD key)
{
	// Do the action now or schedule it for later.
	LOG_MSG( "CTaksiHotKeys::DoHotKey: VKEY_* (%d=%x) %s." LOG_CR, eHotKey, key, evt == HKEVENT_PRESS ? "pressed" : "released");

	bool activated = !hotkeys_pressed[eHotKey] && evt == HKEVENT_PRESS;
	hotkeys_pressed[eHotKey] = evt == HKEVENT_PRESS;

	switch(eHotKey)
	{
	case HOTKEY_Overlay:
		if (activated)
			ToggleOverlay(OVERLAY_HIGHLIGHTER);
		return true;

	case HOTKEY_Stream:
		if (activated)
			ToggleOverlay(OVERLAY_STREAMING);
		return true;

	case HOTKEY_Screenshot:
	case HOTKEY_Bookmark:
	case HOTKEY_StartStopStream:
		// schedule to be in the PresentFrameBegin() call.
		if (activated)
			ScheduleHotKey(eHotKey);
		return false;
	}
	// shouldnt get here!
	ASSERT(0);
}

void CTaksiHotKeys::AddEvent( HOTKEY_TYPE eHotKey, HOTKEY_EVENT eEvent )
{
	HotKeyEvent ev;
	ev.key = eHotKey;
	ev.event = eEvent;
	m_events.push_back( ev );
}

HRESULT CTaksiHotKeys::AttachHotKeysToApp()
{
	// Use DirectInput, if configured so, 
	// Otherwise use thread-specific keyboard hook.
	// Called from PresentFrameBegin()

	ASSERT(!m_bAttachedHotKeys);
	DEBUG_MSG( "CTaksiProcess::AttachHotKeysToApp" LOG_CR );
	m_bAttachedHotKeys = true;	// keyboard configuration done

#ifdef USE_INPUT_HOOKS
	HookInput( );
#endif

#ifdef USE_DIRECTI
	g_UserDI.Hook( );
	if ( g_bUseDirectInput )
	{
		if ( g_UserDI.SetupDirectInput( ) == S_OK )
		{
			// install dummy keyboard hook so that we don't get unmapped,
			// when going to exclusive mode (i.e. unhooking CBT)
			g_UserKeyboard.InstallHookKeys( true );
			
			return S_OK;
		}
		LOG_MSG( "AttachHotKeysToApp: DirectInput init failed" LOG_CR);
	}
#endif
	// if we're not done at this point, use keyboard hook
	// install keyboard hook 
	if ( g_bUseKeyboard )
	{
		if ( !g_UserKeyboard.InstallHookKeys( false ) )
		{
			LOG_MSG( "AttachHotKeysToApp: keyboard hook installation failed" LOG_CR );
			return HRes_GetLastErrorDef(MK_E_MUSTBOTHERUSER);
		}
	}
	
	return S_OK;
}

void CTaksiHotKeys::DetachHotKeys()
{
#ifdef USE_INPUT_HOOKS
	UnhookInput( );
#endif
#ifdef USE_DIRECTI
	// release DirectInput objects 
	g_UserDI.Unhook( );
	g_UserDI.CloseDirectInput();
#endif
	g_UserKeyboard.UninstallHookKeys();	// uninstall keyboard hook
	m_bAttachedHotKeys = false;
}

bool  CTaksiHotKeys::HotkeysAttached( void )
{
	bool hooked = false;
#ifdef USE_DIRECTI
	// check dinput hook
	hooked = g_UserDI.IsHooked( );
#endif
	// check keyboard hook
#ifndef USE_KEYBOARD_HOOK
	hooked = m_bAttachedHotKeys;
#else
	hooked = (hooked || g_UserKeyboard.IsHooked( ));
#endif

	return hooked;
}