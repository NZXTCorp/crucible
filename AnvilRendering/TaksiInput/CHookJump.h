//
// CHookJump.h
//
#pragma once 

#include "TaksiInput.h"
#include "MinHook.h"

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
	}
	void SwapReset(LPVOID)
	{
	}
	template <typename Res, typename ... Args, typename ... CallArgs>
	Res Call(Res(*func)(Args ... args), CallArgs ... args)
	{
		return ((decltype(func))original)(std::forward<CallArgs>(args)...);
	}
#ifndef _WIN64
	template <typename Res, typename ... Args, typename ... CallArgs>
	Res Call(Res(__stdcall *func)(Args ... args), CallArgs ... args)
	{
		return ((decltype(func))original)(std::forward<CallArgs>(args)...);
	}
#endif
public:
	LPVOID original;
	static bool installed;
};

