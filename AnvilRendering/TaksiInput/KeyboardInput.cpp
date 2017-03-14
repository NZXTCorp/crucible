// [KeyboardInput.cpp 2014-03-11 abright]
// keyboard input/capture for overlay

#include "stdafx.h"

#include "TaksiInput.h"
#include "HotKeys.h"

#include "KeyboardInput.h"

#include <vector>

#include <commctrl.h>	// HOTKEYF_ALT

// keyboard state from GetKeyboardState
static bool s_keys[256];
static bool s_pre_overlay_keys[256];

std::vector<KeyEvent> g_keyevents;
CRITICAL_SECTION g_keyeventlock;

extern bool g_bBrowserShowing;

/*KeyInputMsg_KeyEventType KeyInputEventForKeyEvent( KeyEventType event )
{
	switch ( event )
	{
		case KEY_DOWN:
			return KeyInputMsg_KeyEventType_KEYDOWN;
		case KEY_UP:
			return KeyInputMsg_KeyEventType_KEYUP;
		case KEY_CHAR:
			return KeyInputMsg_KeyEventType_CHAR;
	}
	return KeyInputMsg_KeyEventType_CHAR; // should we have an 'unknown' type here? this is just to silence warnings anyway...
}*/

KeyEventType EventForMessage( UINT msg )
{
	switch ( msg )
	{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			return KEY_DOWN;
		case WM_KEYUP:
		case WM_SYSKEYUP:
			return KEY_UP;
		case WM_CHAR:
			return KEY_CHAR;
	}

	return KEY_UNKNOWN;
}

KeyEvent::KeyEvent( int vk, KeyEventType event_type )
{
	vk_code = vk;
	event = event_type;
}

KeyEvent::KeyEvent( UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	event = EventForMessage( uMsg );
	vk_code = wParam;
	scan_code = lParam;
	is_sys_key = (uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP);
}

/*void KeyEvent::CopyToKeyInputMsg( KeyInputMsg *msg )
{
	msg->set_windows_key_code( vk_code );
	msg->set_system_key_code( 0 );
	msg->set_is_system_key( is_sys_key );
	msg->set_type( KeyInputEventForKeyEvent( event ) );
	msg->set_modifiers(0);
}*/

void InitKeyboardInput( void )
{
	InitializeCriticalSection( &g_keyeventlock );	
}

void CleanupKeyboardInput( void )
{
	DeleteCriticalSection( &g_keyeventlock );
}

// add a key up/down event to queue
void AddKeyEvent( int vk_code, KeyEventType event_type )
{
	//if ( !g_Proc.m_bBrowserCreated )
		return;

	KeyEvent event( vk_code, event_type );
	EnterCriticalSection( &g_keyeventlock );
	g_keyevents.push_back( event );
	LeaveCriticalSection( &g_keyeventlock );
}

void AddWMKeyEvent( UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	//if ( !g_Proc.m_bBrowserCreated )
		return;

	KeyEvent event( uMsg, wParam, lParam );
	EnterCriticalSection( &g_keyeventlock );
	g_keyevents.push_back( event );
	LeaveCriticalSection( &g_keyeventlock );
}

// take events from our queue and build a message queue of them to feed to Ember
void MoveKeyEventsToMessageQueue( MessageQueue &queue )
{
	//if ( g_keyevents.empty( ) || !g_Proc.m_bBrowserCreated )
		return;

	EnterCriticalSection( &g_keyeventlock );
	for ( auto ev: g_keyevents )
	{
		/*MessageWrapper *msg = queue.add_msg( );
		msg->set_type( MessageWrapper_Type_KEYINPUT );
		KeyInputMsg *key = msg->mutable_key( );
		ev.CopyToKeyInputMsg( key );*/
	}
	g_keyevents.clear( );
	LeaveCriticalSection( &g_keyeventlock );
}

static bool Pressed(PBYTE keys, BYTE key)
{
	return (keys[key] & 0x80) == 0x80;
}

static bool Pressed(bool *keys, BYTE key)
{
	return keys[key];
}

template <typename T>
static int Modifiers(T keys)
{
		return ((Pressed(keys, VK_CONTROL) || Pressed(keys, VK_LCONTROL) ? HOTKEYF_CONTROL : 0) |
			(Pressed(keys, VK_MENU) || Pressed(keys, VK_LMENU) || Pressed(keys, VK_RMENU) ? HOTKEYF_ALT : 0) |
			(Pressed(keys, VK_SHIFT) || Pressed(keys, VK_LSHIFT) || Pressed(keys, VK_RSHIFT) ? HOTKEYF_SHIFT : 0));
}

template <typename T, typename U>
static void HandleHotkeys(T s_keys, U keys)
{
	auto mods = Modifiers(keys);
	auto prev_mods = Modifiers(s_keys);
	for (int i = 0; i < HOTKEY_QTY; i++)
	{
		auto type = static_cast<HOTKEY_TYPE>(i);

		auto vk = GetHotKey(type);
		if (!vk)
			continue;

		bool pressed = Pressed(keys, vk) && HotkeyModifiersMatch(type, mods);
		bool was_pressed = Pressed(s_keys, vk) && HotkeyModifiersMatch(type, prev_mods);

		if (pressed && !was_pressed && !(g_HotKeys.DoHotKey(type, HKEVENT_PRESS, vk | (mods << 8))))
			g_HotKeys.AddEvent(type, HKEVENT_PRESS);
		else if (!pressed && was_pressed && !(g_HotKeys.DoHotKey(type, HKEVENT_RELEASE, vk | (prev_mods << 8))))
			g_HotKeys.AddEvent(type, HKEVENT_RELEASE);
	}
}

extern bool quick_selecting;

// compare keyboard provided state to last known state. 
void UpdateKeyboardState( PBYTE keys )
{
	HandleHotkeys(s_keys, keys);

	BYTE cancel;
	BYTE select;
	if (quick_selecting) {
		cancel = GetHotKey(HOTKEY_Cancel);
		select = GetHotKey(HOTKEY_Select);
	}

	for ( int i = 0; i < 256; i++ )
	{
		bool key = (keys[i] & 0x80) == 0x80;
		if ( s_keys[i] != key ) // if changed, we'll be generating an event from it
		{
			if ( key && !s_keys[i] )
			{
				//LOG_MSG( "UpdateKeyboardState: key %u down"LOG_CR, i );
				if ( g_bBrowserShowing )
					AddKeyEvent( i, KEY_DOWN );
			}
			else
			{
				//LOG_MSG( "UpdateKeyboardState: key %u up"LOG_CR, i );
				if ( g_bBrowserShowing )
					AddKeyEvent( i, KEY_UP );
			}
		}
		s_keys[i] = key;

		// overlay showing, zero out the 'key down' bit so game sees key never pressed
		if ( g_bBrowserShowing ||
			(quick_selecting && i && (cancel == i || select == i)))
		{
			keys[i] = 0;
		}
	}
}

static bool filter_quick_select(int key)
{
	if (!quick_selecting || !key)
		return false;

	return GetHotKey(HOTKEY_Cancel) == key || GetHotKey(HOTKEY_Select) == key;
}

// update the state of a single key from a GetAsyncKeyState call
SHORT UpdateSingleKeyState( int key, SHORT state )
{
	if ( state & 0x8000 ) // key down
	{
		//if ( !s_keys[key] )
			//LOG_MSG( "UpdateSingleKeyState: key %u down"LOG_CR, key );

		s_keys[key] = true;
	}
	else
	{
		//if ( s_keys[key] )
			//LOG_MSG( "UpdateSingleKeyState: key %u up"LOG_CR, key );

		s_keys[key] = false;
	}

	if (!g_bBrowserShowing && !filter_quick_select(key))
		s_pre_overlay_keys[key] = s_keys[key];
	else
		return s_pre_overlay_keys[key] ? 0x8000 : 0;

	return state;
}

// update the state of a single key from a GetRawInputData call
void UpdateRawKeyState( PRAWKEYBOARD event )
{
	auto type = KEY_UNKNOWN;
	switch (event->Message) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		type = KEY_DOWN;
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		type = KEY_UP;
		break;
	case WM_CHAR:
		type = KEY_CHAR;
		break;
	}

	UpdateWMKeyState(event->VKey, type);
	if (!g_bBrowserShowing && !filter_quick_select(event->VKey))
		return;

	ZeroMemory(event, sizeof(*event));
	//LOG_MSG( "UpdateRawKeyState: key event for %u, flags 0x%x"LOG_CR, event->VKey, event->Flags );
}

static bool s_keys_prev[256];

bool UpdateWMKeyState( int key, KeyEventType type )
{
	memcpy(s_keys_prev, s_keys, sizeof(s_keys_prev));

	// wParam is virtual key code. lParam is a bunch of flags that msdn explains (that don't matter to us). key_down tells us if it was a key down or up message so we can update saved state.
	switch ( type )
	{
		case KEY_DOWN:
			//if ( !s_keys[key] )
				//LOG_MSG( "UpdateWMKeyState: key %u down"LOG_CR, key );
			s_keys[key] = true;
			HandleHotkeys(s_keys_prev, s_keys);
		break;
		case KEY_UP:
			//if ( s_keys[key] )
				//LOG_MSG( "UpdateWMKeyState: key %u up"LOG_CR, key );
			s_keys[key] = false;
			HandleHotkeys(s_keys_prev, s_keys);
		break;
		case KEY_CHAR:
			//LOG_MSG( "UpdateWMKeyState: key %u pressed (char)"LOG_CR, key );
			AddKeyEvent( key, KEY_CHAR );
		break;
	}		
	
	return g_bBrowserShowing || filter_quick_select(key);
}