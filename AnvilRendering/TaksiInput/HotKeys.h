//
// HotKeys.h
//
#ifndef _INC_Input
#define _INC_Input
#if _MSC_VER > 1000
#pragma once
#endif

#include "TaksiInput.h"

#include <vector>

#ifdef USE_DIRECTI
struct CTaksiDI : public CDllFile
{
	// Direct input keys.
	// NOTE: Process/thread specific interface.
public:
	CTaksiDI();

	HRESULT SetupDirectInput();
	void ProcessDirectInput();
	void CloseDirectInput();

	bool Hook( void );
	void Unhook( void );

	bool IsHooked( void );
public:
	bool m_bSetup;
private:
	// key states (for DirectInput) 
	BYTE m_bScanExt[3];	// VK_SHIFT, VK_CONTROL, VK_MENU scan codes.
	bool m_abHotKey[HOTKEY_QTY];
};
extern CTaksiDI g_UserDI;
#endif // USE_DIRECTI

struct CTaksiKeyboard
{
	// keyboard hook
	// (Use ONLY If the DI interface doesnt work)
	// NOTE: Process and Thread-specific keyboard hook.
public:
	CTaksiKeyboard()
		: m_hHookKeys(NULL)
		, m_bHotMask(0)
	{
	}
	bool InstallHookKeys(bool bDummy);
	void UninstallHookKeys(void);
	bool IsHooked( void );
protected:
	static LRESULT CALLBACK DummyKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);

public:
	// keyboard hook handle
	HHOOK m_hHookKeys;
	BYTE m_bHotMask; // the state of HOTKEYF_CONTROL and HOTKEYF_SHIFT
	bool m_bKeysPressed[HOTKEY_QTY]; // hotkey state flags to prevent spamming keypress events
};
extern CTaksiKeyboard g_UserKeyboard;

// hotkey event info we'll use in the new queue
struct HotKeyEvent
{
	HOTKEY_TYPE key;
	HOTKEY_EVENT event;
};

struct CTaksiHotKeys
{
	// Taksi DLL HotKey state information
	// Changed by user pressing specific keys.
public:
	CTaksiHotKeys( ): m_bAttachedHotKeys(false), m_dwHotKeyMask(0), m_events(30) // shouldn't need a huge input queue, there's only a few keys to worry about
	{
	}

	void ScheduleHotKey( HOTKEY_TYPE eHotKey )
	{
		// process the key when we get around to it.
		m_dwHotKeyMask |= (1<<eHotKey);
	}

	HRESULT AttachHotKeysToApp();	// to current app/process.
	void DetachHotKeys();

	bool HotkeysAttached( void );

	bool DoHotKey( HOTKEY_TYPE eHotKey );

	void AddEvent( HOTKEY_TYPE eHotKey, HOTKEY_EVENT eEvent );
public:
	bool m_bAttachedHotKeys;	// hooked the keyboard or DI for this process.
	DWORD m_dwHotKeyMask;	// HOTKEY_TYPE mask

	std::vector<HotKeyEvent> m_events;
};
extern CTaksiHotKeys g_HotKeys;

#endif
