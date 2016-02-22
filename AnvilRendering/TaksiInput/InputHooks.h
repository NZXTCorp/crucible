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

#endif