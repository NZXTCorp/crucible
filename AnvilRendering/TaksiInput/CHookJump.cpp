//
// CHookJump.cpp
//
#include "stdafx.h"
#include "CHookJump.h"

bool CHookJump::InstallHook( LPVOID pFunc, LPVOID pFuncNew )
{
	ASSERT(pFuncNew);
	if ( pFunc == NULL )
	{
		LOG_WARN( "InstallHook: NULL." LOG_CR );
		return false;
	}
	if (IsHookInstalled())
	{
		LOG_WARN( "InstallHook: hook already installed." LOG_CR );
		return true;
	}

	// DEBUG_TRACE(("InstallHook: pFunc = %08x, pFuncNew = %08x" LOG_CR, (UINT_PTR)pFunc, (UINT_PTR)pFuncNew ));

	hook_init(&hook, pFunc, pFuncNew, "unknown hook name");
	rehook(&hook);

	installed = hook.hooked;

	DEBUG_MSG(("InstallHook: JMP-hook planted." LOG_CR));
	return true;
}

void CHookJump::RemoveHook( LPVOID pFunc)
{
	if (pFunc == NULL)
		return;
	if ( ! IsHookInstalled())	// was never set!
		return;
	try 
	{
		unhook(&hook);

		installed = false;
	}
	catch (...)
	{
		DEBUG_ERR(("CHookJump::RemoveHook FAIL" LOG_CR));
	}
}
