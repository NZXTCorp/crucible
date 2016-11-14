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
using namespace std;

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

static unique_ptr<Bitmap> LoadBitmapFromResource( wchar_t *resource_name )
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

	unique_ptr<Gdiplus::Bitmap> image{ Gdiplus::Bitmap::FromStream(pStream) };
    pStream->Release( );

	return image;
}

static Color popupBGColor = Color(192, 0, 0, 0);
static Color textColor = Color(255, 255, 255, 255);

static unique_ptr<Bitmap> CreateMicIndicator(int indicatorID, int w, int h, bool live = false)
{
	int bitmapWidth = w, bitmapHeight = h;
	int iconAdjust = 0;

	if (live) {
		bitmapHeight += 24;
		iconAdjust = 24;
	}

	auto tmp = make_unique<Bitmap>(bitmapWidth, bitmapHeight, PixelFormat32bppARGB);
	Graphics graphics(tmp.get());

	auto colorBar = LoadBitmapFromResource(MAKEINTRESOURCE(IDB_COLOR_BAR));
	auto liveIcon = LoadBitmapFromResource(MAKEINTRESOURCE(IDB_LIVE_ICON));
	auto micIcon = LoadBitmapFromResource(MAKEINTRESOURCE(s_image_res[indicatorID]));

	graphics.Clear(popupBGColor);
	graphics.DrawImage(colorBar.get(), 0, 0, w, h);
	graphics.DrawImage(micIcon.get(), 0, 0 + iconAdjust, w, h);
	if (live) graphics.DrawImage(liveIcon.get(), 0, 4, liveIcon->GetWidth(), liveIcon->GetHeight());

	return tmp;
}

// It doesn't actually matter if the user has deleted Arial, GDI+ will pick the default Windows font if that's the case.
wstring fontFace = L"Arial";
REAL sizeSmall = 10, sizeMedium = 14, sizeLarge = 18;

wstring capturingCaption = L"Forge is now enabled!";
wstring cacheLimitCaption = L"Forge stopped recording";
wstring bookmarkCaption = L"Bookmark created!";
wstring streamStoppedCaption = L"Stream ended";
wstring streamStartedCaption = L"You started streaming";
wstring clipUploadingCaption = L"Uploading clip...";
wstring clipUploadedCaption = L"Clip uploaded!";

wstring bookmarkDescription = L"View it under the Bookmarks tab in the\nForge app when you are done playing.";
wstring cacheLimitDescription = L"We ran out of space to record further.";
wstring streamStartedDescription = L"You will keep streaming until you click\nthe Stop Stream button in the Forge app.";
wstring clipUploadedDescription = L"A link to your clip has been copied to your clipboard.";

wstring hotkeyHelpText[HOTKEY_QTY] = {
	L"Save a screenshot",
	L"Save a moment to upload a clip of later",
	L"Open the in-game clipper",
	L"Open the streaming overlay",
	L"Start or stop streaming",
	L"Mute or unmute microphone",
};

unsigned int indicatorHotkey_Keycode[HOTKEY_QTY];
bool indicatorHotkey_CONTROL[HOTKEY_QTY], indicatorHotkey_MENU[HOTKEY_QTY], indicatorHotkey_SHIFT[HOTKEY_QTY];
bool hotkeysChanged = false;

wstring GetKeyName(unsigned int virtualKey)
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

static unique_ptr<Bitmap> CreatePopupImage(wstring *caption, wstring *desc, unsigned int iconID = 0, unsigned int colorbarID = IDB_COLOR_BAR)
{
	Font largeFont(fontFace.c_str(), sizeLarge, FontStyleBold);
	Font mediumFont(fontFace.c_str(), sizeMedium);

	HDC tempHDC = CreateCompatibleDC(NULL);
	Graphics measureTemp(tempHDC);

	int width = 0, height = 0;
	int iconWidth = 0, iconHeight = 0;

	unique_ptr<Bitmap> colorBar, popupIcon;

	RectF bound;

	measureTemp.MeasureString(caption->c_str(), caption->length(), &largeFont, PointF(0.0f, 0.0f), &bound);
	width = (int)bound.Width;
	height += (int)bound.Height;

	if (desc) {
		measureTemp.MeasureString(desc->c_str(), desc->length(), &mediumFont, PointF(0.0f, 0.0f), &bound);
		if ((int)bound.Width > width) width = (int)bound.Width;
		height += (int)bound.Height;
	}

	measureTemp.ReleaseHDC(tempHDC);

	colorBar = LoadBitmapFromResource(MAKEINTRESOURCE(IDB_COLOR_BAR));
	if (iconID != 0) {
		popupIcon = LoadBitmapFromResource(MAKEINTRESOURCE(iconID));
		iconWidth = popupIcon->GetWidth();
		iconHeight = popupIcon->GetHeight();
	}

	width = width + 64 + iconWidth;
	height = height + 32;
	if (height < iconHeight + 32) height = iconHeight + 32;

	auto tmp = make_unique<Bitmap>(width, height, PixelFormat32bppARGB);
	Graphics graphics(tmp.get());

	SolidBrush brush(textColor);

	graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
	graphics.SetCompositingMode(CompositingModeSourceOver);
	graphics.SetSmoothingMode(SmoothingModeHighQuality);

	graphics.Clear(popupBGColor);
	graphics.DrawImage(colorBar.get(), width - 48, 0, 48, 48);
	if (iconID != 0) graphics.DrawImage(popupIcon.get(), 16, 16, iconWidth, iconHeight);
	graphics.DrawString(caption->c_str(), caption->length(), &largeFont, PointF(32.0f + iconWidth, 16.0f), &brush);
	if(desc) graphics.DrawString(desc->c_str(), desc->length(), &mediumFont, PointF(32.0f + iconWidth, 32.0f + sizeLarge), &brush);

	return tmp;
}

void SetIndicatorHotkey(int index, int keycode, bool ctrl, bool alt, bool shift)
{
	if (index > HOTKEY_QTY) return;

	if (indicatorHotkey_Keycode[index] != keycode || indicatorHotkey_CONTROL[index] != ctrl ||
		indicatorHotkey_MENU[index] != alt || indicatorHotkey_SHIFT[index] != ctrl)  hotkeysChanged = true;

	indicatorHotkey_Keycode[index] = keycode;
	indicatorHotkey_CONTROL[index] = ctrl;
	indicatorHotkey_MENU[index] = alt;
	indicatorHotkey_SHIFT[index] = shift;
}

IndicatorManager::IndicatorManager( void )
{
	for ( int i = 0; i < INDICATE_NONE; i++ )
		m_images[i] = nullptr;
}

IndicatorManager::~IndicatorManager( void )
{
	FreeImages( );
}

wstring MakeHotkeyDescription() {
	bool noHotkeys = true;
	wstring hotKeyDescription;

#define MOD(x) (indicatorHotkey_ ## x[i] ? GetKeyName(VK_ ## x) + L" + " : L"")

	for (int i = 0; i < HOTKEY_QTY; i++) {
		if (indicatorHotkey_Keycode[i] != 0) {
			hotKeyDescription += MOD(CONTROL);
			hotKeyDescription += MOD(MENU);
			hotKeyDescription += MOD(SHIFT);
			hotKeyDescription += GetKeyName(indicatorHotkey_Keycode[i]) + L" - " + hotkeyHelpText[i] + L"\n";

			noHotkeys = false;
		}
	}

	if (!noHotkeys)
		hotKeyDescription = L"Hotkeys:\n" + hotKeyDescription;

	return hotKeyDescription;
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
			m_images[i] = CreatePopupImage(&bookmarkCaption, &bookmarkDescription, IDB_BOOKMARK_ICON);
			break;
		case INDICATE_ENABLED:
		case INDICATE_CAPTURING:
			m_images[i] = CreatePopupImage(&capturingCaption, &MakeHotkeyDescription());
			break;
		case INDICATE_CACHE_LIMIT:
			m_images[i] = CreatePopupImage(&cacheLimitCaption, &cacheLimitDescription, 0, IDB_COLOR_BAR_ERROR);
			break;
		case INDICATE_STREAM_STOPPED:
			m_images[i] = CreatePopupImage(&streamStoppedCaption, NULL, 0);
			break;
		case INDICATE_STREAM_STARTED:
			m_images[i] = CreatePopupImage(&streamStartedCaption, &streamStartedDescription, IDB_CHECKMARK_ICON);
			break;
		case INDICATE_CLIP_PROCESSING:
			m_images[i] = CreatePopupImage(&clipUploadingCaption, NULL, 0);
			break;
		case INDICATE_CLIP_PROCESSED:
			m_images[i] = CreatePopupImage(&clipUploadedCaption, &clipUploadedDescription, IDB_CHECKMARK_ICON);
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

	return true;
}

void IndicatorManager::UpdateImages(void)
{
	if (!hotkeysChanged) return;
	hotkeysChanged = false;

	m_images[INDICATE_ENABLED] = CreatePopupImage(&capturingCaption, &MakeHotkeyDescription());

	updateTextures = true;

	return;
}

void IndicatorManager::FreeImages( void )
{
	for (int i = 0; i < INDICATE_NONE; i++)
		m_images[i].reset();
}

Gdiplus::Bitmap *IndicatorManager::GetImage( int indicator_event )
{
	if ( indicator_event < 0 || indicator_event >= INDICATE_NONE )
		return nullptr;

	// if using the default F5/F6 hotkeys for bookmark/screenshot, we show a different 'enabled' indicator that tells user about them
	/*if ( indicator_event == INDICATE_ENABLED && (g_AnvilConfig.m_wHotKey[HOTKEY_Bookmark] == VK_F5 && g_AnvilConfig.m_wHotKey[HOTKEY_Screenshot] == VK_F6) )
		return m_image_enabled_hotkeys;*/

	// otherwise show the proper image
	return m_images[indicator_event].get();
}