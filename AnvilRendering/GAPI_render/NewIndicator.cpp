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
#include <string>

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
	IDB_BOOKMARK,
	IDB_MIC_ICON_IDLE,
	IDB_MIC_ICON,
	IDB_MIC_ICON_MUTED,
	IDB_CACHE_LIMIT,
	IDB_HIGHLIGHT_UPLOADING,
	IDB_HIGHLIGHT_UPLOADED,
	IDB_STREAM_STARTED,
	IDB_STREAM_STOPPED,
	IDB_STREAMING,
	IDB_MIC_ICON_IDLE,
	IDB_MIC_ICON,
	IDB_MIC_ICON_MUTED,
};

int micIndicatorW = 48, micIndicatorH = 48;

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

static Color popupBGColor = Color(192, 0, 0, 0);
static Color textColor = Color(255, 255, 255, 255);

static Bitmap *CreateMicIndicator(int indicatorID, int w, int h, bool live = false)
{
	int bitmapWidth = w, bitmapHeight = h;
	int iconAdjust = 0;

	if (live) {
		bitmapHeight += 24;
		iconAdjust = 24;
	}

	Bitmap *tmp = new Bitmap(bitmapWidth, bitmapHeight, PixelFormat32bppARGB);
	Graphics graphics(tmp);

	Bitmap *colorBar = LoadBitmapFromResource(MAKEINTRESOURCE(IDB_COLOR_BAR));
	Bitmap *liveIcon = LoadBitmapFromResource(MAKEINTRESOURCE(IDB_LIVE_ICON));
	Bitmap *micIcon = LoadBitmapFromResource(MAKEINTRESOURCE(s_image_res[indicatorID]));

	graphics.Clear(popupBGColor);
	graphics.DrawImage(colorBar, 0, 0, w, h);
	graphics.DrawImage(micIcon, 0, 0 + iconAdjust, w, h);
	if (live) graphics.DrawImage(liveIcon, 0, 4, liveIcon->GetWidth(), liveIcon->GetHeight());

	if (colorBar) DeleteObject(colorBar);
	if (micIcon) DeleteObject(micIcon);
	if (liveIcon) DeleteObject(liveIcon);

	return tmp;
}

// It doesn't actually matter if the user has deleted Arial, GDI+ will pick the default Windows font if that's the case.
wchar_t fontFace[64] = L"Arial";
unsigned int sizeSmall = 10, sizeMedium = 14, sizeLarge = 18;

wchar_t capturingCaption[] = L"Forge is now enabled!";
wchar_t cacheLimitCaption[] = L"Forge stopped recording";
wchar_t bookmarkCaption[] = L"Bookmark created!";
wchar_t streamStoppedCaption[] = L"Stream ended";
wchar_t streamStartedCaption[] = L"You started streaming";
wchar_t clipUploadingCaption[] = L"Uploading clip...";
wchar_t clipUploadedCaption[] = L"Clip uploaded!";

wchar_t bookmarkDescription[] = L"View it under the Bookmarks tab in the\nForge app when you are done playing.";
wchar_t cacheLimitDescription[] = L"We ran out of space to record further.";
wchar_t streamStartedDescription[] = L"You will keep streaming until you click\nthe Stop Stream button in the Forge app.";
wchar_t clipUploadedDescription[] = L"A link to your clip has been copied to your clipboard.";

wchar_t *hotkeyHelpText[] = {
	L"Save a screenshot",
	L"Save a moment to upload a clip of later",
	L"Open the in-game clipper",
	L"Open the stream overlay that never existed",
	L"Start or stop streaming (not really)",
};

unsigned int indicatorHotkey_Keycode[HOTKEY_QTY];
bool indicatorHotkey_Ctrl[HOTKEY_QTY], indicatorHotkey_Alt[HOTKEY_QTY], indicatorHotkey_Shift[HOTKEY_QTY];
bool hotkeysChanged = false;

std::wstring GetKeyName(unsigned int virtualKey)
{
	unsigned int scanCode = MapVirtualKey(virtualKey, MAPVK_VK_TO_VSC);

	// because MapVirtualKey strips the extended bit for some keys
	switch (virtualKey)
	{
	case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: // arrow keys
	case VK_PRIOR: case VK_NEXT: // page up and page down
	case VK_END: case VK_HOME:
	case VK_INSERT: case VK_DELETE:
	case VK_DIVIDE: // numpad slash
	case VK_NUMLOCK:
	{
		scanCode |= 0x100; // set extended bit
		break;
	}
	}

	wchar_t keyName[50];
	if (GetKeyNameText(scanCode << 16, keyName, sizeof(keyName)) != 0)
		return keyName;
	else
		return L"[Error]";
}

static Bitmap *CreatePopupImage(wchar_t *caption, wchar_t *desc, unsigned int iconID = 0, unsigned int colorbarID = IDB_COLOR_BAR)
{
	Font *largeFont = new Font(fontFace, sizeLarge, FontStyleBold);
	Font *mediumFont = new Font(fontFace, sizeMedium);

	HDC tempHDC = CreateCompatibleDC(NULL);
	Graphics measureTemp(tempHDC);

	int width = 0, height = 0;
	int iconWidth = 0, iconHeight = 0;

	Bitmap *colorBar = NULL, *popupIcon = NULL;

	RectF bound;

	measureTemp.MeasureString(caption, wcslen(caption), largeFont, PointF(0.0f, 0.0f), &bound);
	width = (int)bound.Width;
	height += (int)bound.Height;

	measureTemp.MeasureString(desc, wcslen(desc), mediumFont, PointF(0.0f, 0.0f), &bound);
	if ((int)bound.Width > width) width = (int)bound.Width;
	height += (int)bound.Height;

	measureTemp.ReleaseHDC(tempHDC);

	colorBar = LoadBitmapFromResource(MAKEINTRESOURCE(IDB_COLOR_BAR));
	if (iconID != 0) {
		popupIcon = LoadBitmapFromResource(MAKEINTRESOURCE(iconID));
		iconWidth = popupIcon->GetWidth();
		iconHeight = popupIcon->GetHeight();
	}

	width = width + 64 + iconWidth;
	height = height + 32;

	Bitmap *tmp = new Bitmap(width, height, PixelFormat32bppARGB);
	Graphics graphics(tmp);

	SolidBrush brush(textColor);

	graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
	graphics.SetCompositingMode(CompositingModeSourceOver);
	graphics.SetSmoothingMode(SmoothingModeHighQuality);

	graphics.Clear(popupBGColor);
	graphics.DrawImage(colorBar, width - 48, 0, 48, 48);
	if (iconID != 0) graphics.DrawImage(popupIcon, 16, 16, iconWidth, iconHeight);
	graphics.DrawString(caption, wcslen(caption), largeFont, PointF(32.0f + iconWidth, 16.0f), &brush);
	graphics.DrawString(desc, wcslen(desc), mediumFont, PointF(32.0f + iconWidth, 32.0f + sizeLarge), &brush);

	if (colorBar) DeleteObject(colorBar);
	if (popupIcon) DeleteObject(popupIcon);
	if (largeFont) DeleteObject(largeFont);
	if (mediumFont) DeleteObject(mediumFont);

	return tmp;
}

void SetIndicatorHotkey(int index, int keycode, bool ctrl, bool alt, bool shift)
{
	if (index > HOTKEY_QTY) return;

	if (indicatorHotkey_Keycode[index] != keycode || indicatorHotkey_Ctrl[index] != ctrl ||
		indicatorHotkey_Alt[index] != alt || indicatorHotkey_Shift[index] != ctrl)  hotkeysChanged = true;

	indicatorHotkey_Keycode[index] = keycode;
	indicatorHotkey_Ctrl[index] = ctrl;
	indicatorHotkey_Alt[index] = alt;
	indicatorHotkey_Shift[index] = shift;
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

void MakeHotkeyDescription(wchar_t *hotkeyDescription) {
	wcscat(hotkeyDescription, L"Hotkeys:\n");

	for (int i = 0; i < HOTKEY_QTY; i++) {
		if (indicatorHotkey_Keycode[i] != 0) {
			wchar_t tmp[256] = L"";

			wsprintf(tmp, L"%s%s%s%s  -  %s\n",
				(indicatorHotkey_Ctrl[i]) ? L"Ctrl + " : L"",
				(indicatorHotkey_Alt[i]) ? L"Alt + " : L"",
				(indicatorHotkey_Shift[i]) ? L"Shift + " : L"",
				GetKeyName(indicatorHotkey_Keycode[i]).c_str(),
				hotkeyHelpText[i]);

			wcscat(hotkeyDescription, tmp);
		}
	}
}

bool IndicatorManager::LoadImages( void )
{
	for ( int i = 0; i < INDICATE_NONE; i++ )
	{
		switch (i) {
		case INDICATE_STREAM_MIC_ACTIVE:
		case INDICATE_STREAM_MIC_IDLE:
		case INDICATE_STREAM_MIC_MUTED:
			m_images[i] = CreateMicIndicator(i, micIndicatorW, micIndicatorH, true);
			break;
		case INDICATE_MIC_ACTIVE:
		case INDICATE_MIC_IDLE:
		case INDICATE_MIC_MUTED:
			m_images[i] = CreateMicIndicator(i, micIndicatorW, micIndicatorH);
			break;
		case INDICATE_BOOKMARK:
			m_images[i] = CreatePopupImage(bookmarkCaption, bookmarkDescription, IDB_BOOKMARK_ICON);
			break;
		case INDICATE_ENABLED:
		case INDICATE_CAPTURING: {
				wchar_t hotkeyDescription[1024] = L"";

				MakeHotkeyDescription(&hotkeyDescription[0]);

				m_images[i] = CreatePopupImage(capturingCaption, hotkeyDescription);
			}
			break;
		case INDICATE_CACHE_LIMIT:
			m_images[i] = CreatePopupImage(cacheLimitCaption, cacheLimitDescription, 0, IDB_COLOR_BAR_ERROR);
			break;
		case INDICATE_STREAM_STOPPED:
			m_images[i] = CreatePopupImage(streamStoppedCaption, L"", 0);
			break;
		case INDICATE_STREAM_STARTED:
			m_images[i] = CreatePopupImage(streamStartedCaption, streamStartedDescription, IDB_CHECKMARK_ICON);
			break;
		case INDICATE_CLIP_PROCESSING:
			m_images[i] = CreatePopupImage(clipUploadingCaption, L"", 0);
			break;
		case INDICATE_CLIP_PROCESSED:
			m_images[i] = CreatePopupImage(clipUploadedCaption, clipUploadedDescription, IDB_CHECKMARK_ICON);
			break;
		default:
			m_images[i] = LoadBitmapFromResource(MAKEINTRESOURCE(s_image_res[i]));
			if (!m_images[i])
			{
				LOG_MSG("LoadImages: load failed at %d" LOG_CR, i);
				return false;
			}
			break;
		}
	}

	//m_image_enabled_hotkeys = LoadBitmapFromResource( MAKEINTRESOURCE(IDB_ENABLED_HOTKEYS) );

	return true;
}

void IndicatorManager::UpdateImages(void)
{
	if (!hotkeysChanged) return;
	hotkeysChanged = false;

	if (m_images[INDICATE_ENABLED]) {
		DeleteObject(m_images[INDICATE_ENABLED]);
		m_images[INDICATE_ENABLED] = NULL;
	}

	wchar_t hotkeyDescription[1024];
	memset(hotkeyDescription, 0x00, 1024 * sizeof(wchar_t));

	MakeHotkeyDescription(&hotkeyDescription[0]);

	m_images[INDICATE_ENABLED] = CreatePopupImage(capturingCaption, hotkeyDescription);

	updateTextures = true;

	return;
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