// [KeyboardInput.h 2014-03-11 abright]
// keyboard input/capture for overlay

#ifndef KEYBOARD_INPUT_NASTY
#define KEYBOARD_INPUT_NASTY

enum KeyEventType
{
	KEY_UNKNOWN = 0,
	KEY_DOWN,	// WM_KEYDOWN or WM_SYSKEYDOWN, input events
	KEY_UP,		// WM_KEYUP or WM_SYSKEYUP
	KEY_CHAR	// a key press from WM_CHAR
};

class KeyInputMsg;
class KeyEvent
{
public:
	int vk_code;
	int scan_code;
	bool is_sys_key;
	KeyEventType event;

	KeyEvent( int vk_code, KeyEventType event_type );
	KeyEvent( UINT uMsg, WPARAM wParam, LPARAM lParam );

	void CopyToKeyInputMsg( KeyInputMsg *msg );
};

// prepare stuff used for keyboard input
void InitKeyboardInput( void );
// cleanup anything used for keyboard input
void CleanupKeyboardInput( void );

// update key states from GetKeyboardState
void UpdateKeyboardState( PBYTE keys );
// update a single key state from GetAsyncKeyState. returns new value to pass as result of call
SHORT UpdateSingleKeyState( int key, SHORT state );
// update a single key state from GetRawInputData
void UpdateRawKeyState( PRAWKEYBOARD event );
// update single key state from WM_KEYUP or WM_KEYDOWN messages. key_down = true for WM_KEYDOWN, false for WM_KEYUP. returns true if we're eating input to hide it from the game.
bool UpdateWMKeyState( int key, KeyEventType type );

class MessageQueue;
// fill message queue with key events and clear the key event queue
void MoveKeyEventsToMessageQueue( MessageQueue &queue );

#endif