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


template <typename Func>
struct FuncHook
{
	const char *func_name;
	Func *wrapper;
	Func *original = nullptr;
	CHookJump hook;

	FuncHook(const char *func_name, Func *wrapper)
		: func_name(func_name),
		  wrapper(wrapper)
	{}

	bool Install(Func *original_)
	{
		original = original_;

		if (!hook.InstallHook(original_, wrapper))
			return false;

		return true;
	}

	void Remove()
	{
		if (original)
			hook.RemoveHook(original);
	}

	template <typename ... CallArgs>
	auto Call(CallArgs ... args) -> decltype(wrapper(args ...))
	{
		hook.SwapOld(nullptr);
		auto res = hook.Call(original, std::forward<CallArgs>(args) ...);
		hook.SwapReset(nullptr);
		return res;
	}
};

