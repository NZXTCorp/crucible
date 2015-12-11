// [NewIndicator.h 2014-06-30 abright]
// new indicator image and display management

#ifndef NEW_INDICATORS_DIRTY
#define NEW_INDICATORS_DIRTY

#include <gdiplus.h>

// NOTE: these match the order of the resource images!
enum IndicatorEvent
{
	INDICATE_CAPTURING = 0,  // streaming active, everything is ok
	INDICATE_CONNECTING,     // connecting to server/preparing stream (everything between enabled and capturing)
	INDICATE_DISCONNECTED,   // disconnected, something went badly wrong and crucible can't stream (eg: connection error and no more retries)
	INDICATE_ENABLED,        // forge enabled - crucible knows about us and is ready to stream
	INDICATE_RECONNECTING,   // reconnecting - lost connection but we're trying to fix it
	INDICATE_SCREENSHOT,     // screenshot taken, show for a bit then return to previous state
	INDICATE_BOOKMARK,       // bookmark set, show for a bit then return to previous state
	INDICATE_PAUSED,         // stream paused (we're using a placeholder static image instead of frames from game)
	INDICATE_RESUMED,        // stream resumed (back to showing the game)
	INDICATE_WELCOME1,       // welcome 1 (demand a bookmark from user)
	INDICATE_WELCOME2,       // welcome 2 (congratulate user and tell them they can do again)
	INDICATE_REMINDER,       // reminder (remind user they can take more bookmarks)
	INDICATE_MIC_IDLE,       // mic capture ready/idle - we're recording but not mixing it in to the output stream
	INDICATE_MIC_ACTIVE,     // mic capture active - PTT pressed or it's in continuous mode so we're recording and mixing into output stream
	INDICATE_MIC_MUTED,      // mic capture muted (continuous mode) or disabled due to error
	INDICATE_NONE            // no image for this one, just a placeholder for loops and to tell graphics code we're not drawing anything
};

enum TAKSI_INDICATE_TYPE
{
	// Indicate my current state to the viewer
	// Color a square to indicate my mode.
	TAKSI_INDICATE_Idle = 0,	// No app ready and not looking.
	TAKSI_INDICATE_Hooking,		// CBT is hooked. (looking for new app to load)
	TAKSI_INDICATE_Ready,		// current app focus is ready to record?
	TAKSI_INDICATE_Recording,	// actively recording right now
	TAKSI_INDICATE_RecordPaused,
	//TAKSI_INDICATE_Error,		// sit in error state til cleared ??
	TAKSI_INDICATE_QTY,
};

// indicator dimensions/position
#define INDICATOR_X 8
#define INDICATOR_Y 8
#define INDICATOR_Width 16
#define INDICATOR_Height 16

enum IndicatorAnimation
{
	INDICATOR_HIDE = 0,     // hidden, doesn't display anything
	INDICATOR_FADEOUT,      // fading out before being hidden 
	INDICATOR_PULSATE_UP,   // pulsate effect, alpha going up (fading in)
	INDICATOR_PULSATE_DOWN, // pulsate effect, alpha going down (fading out)
	INDICATOR_SHOW          // show with constant alpha. switches to fadeout near stop time
};

enum WelcomeStep
{
	WELCOME_INIT_FADEIN = 0, // fade-in for the initial dialog
	WELCOME_INIT,            // initial welcome dialog, asks user to take a bookmark (and doesn't hide until they do)
	WELCOME_INIT_FADEOUT,    // fade-out of initial dialog
	WELCOME_CONFIRM_FADEIN,  // fade-in of the confirm dialog
	WELCOME_CONFIRM,         // confirm dialog, tells user they can do it again whenever (disappears after 10s)
	WELCOME_CONFIRM_FADEOUT, // fade-out of confirm dialog
	WELCOME_REMINDER,        // reminder indicator we show after 30 seconds or so
	WELCOME_FINISHED         // finished, don't show anything else
};

extern IndicatorEvent IndicatorForWelcomeStep[WELCOME_FINISHED];
extern const DWORD sm_IndColors[TAKSI_INDICATE_QTY];

class IndicatorManager
{
private:
	Gdiplus::Bitmap *m_images[INDICATE_NONE];
	Gdiplus::Bitmap *m_image_enabled_hotkeys; // bigger 'enabled' indicator with info about default hotkeys that we'll swap in if they're set
public:
	IndicatorManager( void );
	~IndicatorManager( void );

	bool LoadImages( void );
	void FreeImages( void );
	Gdiplus::Bitmap *GetImage( int indicator_event );
};

#endif