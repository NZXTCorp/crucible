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
	if ( IsHookInstalled() && ! memcmp(pFunc, m_Jump, sizeof(m_Jump)))
	{
		LOG_WARN( "InstallHook: already has JMP-implant." LOG_CR );
		return true;
	}

	// DEBUG_TRACE(("InstallHook: pFunc = %08x, pFuncNew = %08x" LOG_CR, (UINT_PTR)pFunc, (UINT_PTR)pFuncNew ));
	m_Jump[0] = 0;

	DWORD dwNewProtection = PAGE_EXECUTE_READWRITE;
	if ( ! ::VirtualProtect( pFunc, 8, dwNewProtection, &m_dwOldProtection))
	{
		ASSERT(0);
		LOG_WARN( "InstallHook: couldn't change memory protection (0x%x)" LOG_CR, GetLastError( ) );
		return false;
	}

	// unconditional JMP to relative address is 5 bytes.
	m_Jump[0] = 0xe9;
	DWORD dwAddr = (DWORD)((UINT_PTR)pFuncNew - (UINT_PTR)pFunc) - sizeof(m_Jump);
	DEBUG_TRACE(("JMP %08x" LOG_CR, dwAddr ));
	memcpy(m_Jump+1, &dwAddr, sizeof(dwAddr));

	memcpy(m_OldCode, pFunc, sizeof(m_OldCode));
	memcpy(pFunc, m_Jump, sizeof(m_Jump));

	DEBUG_MSG(("InstallHook: JMP-hook planted." LOG_CR));
	return true;
}

void CHookJump::RemoveHook( LPVOID pFunc )
{
	if (pFunc == NULL)
		return;
	if ( ! IsHookInstalled())	// was never set!
		return;
	try 
	{
		memcpy(pFunc, m_OldCode, sizeof(m_OldCode));	// SwapOld(pFunc)
		DWORD dwOldProtection = 0;
		::VirtualProtect(pFunc, 8, m_dwOldProtection, &dwOldProtection ); // restore protection.
		m_Jump[0] = 0;	// destroy my jump. (must reconstruct it)
	}
	catch (...)
	{
		DEBUG_ERR(("CHookJump::RemoveHook FAIL" LOG_CR));
	}
}

bool CHookJump::VerifyHook( const LPVOID pFunc ) const
{
	return (IsHookInstalled() && !memcmp(m_Jump, pFunc, sizeof(m_Jump)));
}
