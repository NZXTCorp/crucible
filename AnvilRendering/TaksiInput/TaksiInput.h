#pragma once

#include "stdafx.h"
#include "AnvilRendering.h"
#include "GAPI_Render/CDll.h"
#include "GAPI_Render/IRefPtr.h"
#include "CHookJump.h"

#ifndef COUNTOF			// similar to MFC _countof()
#define COUNTOF(a) 		(sizeof(a)/sizeof((a)[0]))	// dimensionof() ? = count of elements of an array
#endif

#ifndef ASSERT
#define ASSERT(x)
#endif

inline HRESULT HRes_GetLastErrorDef(HRESULT hResDefault)
{
	// Something failed so find out why
	// hResDefault = E_FAIL or CONVERT10_E_OLESTREAM_BITMAP_TO_DIB
	DWORD dwLastError = ::GetLastError();
	if (dwLastError == 0)
		dwLastError = hResDefault; // no idea why this failed.
	return HRESULT_FROM_WIN32(dwLastError);
}