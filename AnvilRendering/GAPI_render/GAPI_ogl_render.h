// [GAPI_ogl_render.h 2014-07-26 abright]
// new home for opengl rendering

#ifndef GAPI_OGL_RENDER_NASTY
#define GAPI_OGL_RENDER_NASTY

class OpenGLRenderer
{
private:
	unsigned int m_uTexID;
	unsigned int m_uTexIDIndicators[INDICATE_NONE];
	SIZE m_sIndicatorSize[INDICATE_NONE];

	void *m_buffer;
public:
	OpenGLRenderer( void );
	~OpenGLRenderer( void );

	// init all the d3d objects we'll need (including indicator textures)
	bool InitRenderer( IndicatorManager &manager );
	bool UpdateIndicatorImages(IndicatorManager &manager);
	// release/free everything. could be unhooking or a device reset.
	void FreeRenderer( void );

	// draw the old indicator
	void DrawIndicator( TAKSI_INDICATE_TYPE eIndicate );
	// draw the new indicator images
	void DrawNewIndicator( IndicatorEvent eIndicatorEvent, BYTE alpha );
	// draw overlay
	void DrawOverlay( void );
};

bool LoadOpenGLFunctions();

#endif 