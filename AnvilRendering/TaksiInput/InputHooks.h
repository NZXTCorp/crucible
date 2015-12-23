// [InputHooks.h 2014-03-12 abright]
// new home for input hooking code

#ifndef INPUT_HOOK_NASTY
#define INPUT_HOOK_NASTY

bool HookInput( void );
void UnhookInput( void );

#ifdef USE_DIRECTI
bool HookDI( UINT_PTR dll_base );
void UnhookDI( void );
#endif

// process window messages relating to input. return true if we should eat the message (return 0 from HookedWndProc instead of calling the game's original handler)
bool InputWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );

#endif