// [NewIndicator.cpp 2014-06-30 abright]
// new indicator image and display management

#include "stdafx.h"
#include "../AnvilRendering.h"
//#include "TaksiDll.h"
//#include "GAPI_Base.h"

#include <Shlwapi.h>
#include <gdiplus.h>
#include "NewIndicator.h"

#include "resource.h"

using namespace Gdiplus;

// D3DCOLOR format is high to low, Alpha, Blue, Green, Red
const DWORD sm_IndColors[TAKSI_INDICATE_QTY] =
{
	// RGB() sort of
	0xff888888,	// TAKSI_INDICATE_Idle
	0xff4488fe,	// TAKSI_INDICATE_Hooked = Blue,
	0xff88fe00,	// TAKSI_INDICATE_Ready = green
	0xfffe4400,	// TAKSI_INDICATE_Recording = red.
	0xff444444,	// TAKSI_INDICATE_RecordPaused = Gray
};

static int s_image_res[INDICATE_NONE] = 
{  
	IDB_CAPTURING,
	IDB_ENABLED,
	IDB_MIC_IDLE,
	IDB_MIC_ACTIVE,
	IDB_MIC_MUTE
};

static Bitmap *LoadBitmapFromResource( wchar_t *resource_name )
{
/*#ifdef _WIN64
	HMODULE hMod = GetModuleHandle( L"Anvil64.dll" );
#else
	HMODULE hMod = GetModuleHandle( L"AnvilRendering.dll" );
#endif*/

	HMODULE hMod;
	GetModuleHandleEx(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCTSTR)LoadBitmapFromResource, &hMod);

    HRSRC hRes = FindResource( hMod, resource_name, L"PNG" );
    if ( !hRes ) 
	{
		LOG_MSG( "LoadBitmapFromResource: FindResource failed! %u" LOG_CR, GetLastError( ) );
        return nullptr;
	}

    HGLOBAL hData = LoadResource( hMod, hRes );
    if ( !hData )
	{
		LOG_MSG( "LoadBitmapFromResource: LoadResource failed! %u" LOG_CR, GetLastError( ) );
        return nullptr;
	}

    void *data = LockResource( hData );
    DWORD size = SizeofResource( hMod, hRes );
	
    IStream *pStream = SHCreateMemStream( (BYTE *)data, (UINT)size );

    if ( !pStream )
	{
		LOG_MSG( "LoadBitmapFromResource: SHCreateMemStream failed! %u" LOG_CR, GetLastError( ) );
        return nullptr;
	}

    Gdiplus::Bitmap *image = Gdiplus::Bitmap::FromStream( pStream );
    pStream->Release( );

	return image;
}

IndicatorManager::IndicatorManager( void )
{
	for ( int i = 0; i < INDICATE_NONE; i++ )
		m_images[i] = nullptr;

	m_image_enabled_hotkeys = nullptr;
}

IndicatorManager::~IndicatorManager( void )
{
	FreeImages( );
}

bool IndicatorManager::LoadImages( void )
{
	for ( int i = 0; i < INDICATE_NONE; i++ )
	{
		//LOG_MSG( "LoadImages: loading image %d" LOG_CR, IDB_FLASHBACK+i );
		m_images[i] = LoadBitmapFromResource( MAKEINTRESOURCE(s_image_res[i]) );
		if ( !m_images[i] )
		{
			LOG_MSG( "LoadImages: load failed at %d" LOG_CR, i );
			return false;
		}
	}

	m_image_enabled_hotkeys = LoadBitmapFromResource( MAKEINTRESOURCE(IDB_ENABLED_HOTKEYS) );

	return true;
}

void IndicatorManager::FreeImages( void )
{
	for ( int i = 0; i < INDICATE_NONE; i++ )
	{
		if ( !m_images[i] )
			continue;

		delete m_images[i];
		m_images[i] = nullptr;
	}

	if ( m_image_enabled_hotkeys )
	{
		delete m_image_enabled_hotkeys;
		m_image_enabled_hotkeys = nullptr;
	}
}

Gdiplus::Bitmap *IndicatorManager::GetImage( int indicator_event )
{
	if ( indicator_event < 0 || indicator_event >= INDICATE_NONE )
		return nullptr;

	// if using the default F5/F6 hotkeys for bookmark/screenshot, we show a different 'enabled' indicator that tells user about them
	/*if ( indicator_event == INDICATE_ENABLED && (g_AnvilConfig.m_wHotKey[HOTKEY_Bookmark] == VK_F5 && g_AnvilConfig.m_wHotKey[HOTKEY_Screenshot] == VK_F6) )
		return m_image_enabled_hotkeys;*/

	// otherwise show the proper image
	return m_images[indicator_event];
}