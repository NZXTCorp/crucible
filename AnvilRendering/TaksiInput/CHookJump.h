//
// CHookJump.h
//
#pragma once 

#include "TaksiInput.h"
#include "../funchook.h"

#ifndef ASSERT
#define ASSERT(x)
#endif

struct TAKSI_LINK CHookJump
{
public:
	CHookJump() = default;

	bool IsHookInstalled() const
	{
		return installed;
	}
	bool InstallHook( LPVOID pFunc, LPVOID pFuncNew );
	void RemoveHook( LPVOID pFunc );
	
	void SwapOld(LPVOID)
	{
		if (!IsHookInstalled())
			return;

		unhook(&hook);
	}
	void SwapReset(LPVOID)
	{
		// put back JMP instruction again
		if ( ! IsHookInstalled())	// hook has since been destroyed!
			return;

		rehook(&hook);
	}
	template <typename Res, typename ... Args, typename ... CallArgs>
	Res Call(Res(*func)(Args ... args), CallArgs ... args)
	{
		return ((decltype(func))hook.call_addr)(std::forward<CallArgs>(args)...);
	}
#ifndef _WIN64
	template <typename Res, typename ... Args, typename ... CallArgs>
	Res Call(Res(__stdcall *func)(Args ... args), CallArgs ... args)
	{
		return ((decltype(func))hook.call_addr)(std::forward<CallArgs>(args)...);
	}
#endif
public:
	func_hook hook{};
	bool installed = false;
};

