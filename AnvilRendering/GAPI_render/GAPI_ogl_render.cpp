// [GAPI_ogl_render.cpp 2014-07-26 abright]
// new home for opengl rendering

#include "stdafx.h"

#include "CImageGDIP.h"
#include "NewIndicator.h"

#include "../AnvilRendering.h"
//#include "TaksiDll.h"
//#include "GAPI_Base.h"

#include "../../Crucible/scopeguard.hpp"

#include <array>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include "glext.h"

//#include "GAPI_ogl.h"
#include "GAPI_ogl_render.h"

#include "TextureBufferingHelper.hpp"

#define GAPIOGLFUNC(a,b,c) \
	typedef WINGDIAPI b (APIENTRY * PFN##a) c;\
	PFN##a s_##a = nullptr;
#include "GAPI_OGL.TBL"
#undef GAPIOGLFUNC

PFNGLACTIVETEXTUREARBPROC s_glActiveTextureARB;
PFNGLBINDBUFFERPROC s_glBindBuffer;
PFNGLGENBUFFERSPROC s_glGenBuffers;
PFNGLDELETEBUFFERSPROC s_glDeleteBuffers;
PFNGLBUFFERDATAPROC s_glBufferData;
PFNGLVERTEXATTRIBPOINTERPROC s_glVertexAttribPointer;
PFNGLENABLEVERTEXATTRIBARRAYPROC s_glEnableVertexAttribArray;
PFNGLBINDVERTEXARRAYPROC s_glBindVertexArray;
PFNGLGENVERTEXARRAYSPROC s_glGenVertexArrays;
PFNGLDELETEVERTEXARRAYSPROC s_glDeleteVertexArrays;

PFNGLCREATESHADERPROC s_glCreateShader;
PFNGLDELETESHADERPROC s_glDeleteShader;
PFNGLATTACHSHADERPROC s_glAttachShader;
PFNGLBINDATTRIBLOCATIONPROC s_glBindAttribLocation;
PFNGLCOMPILESHADERPROC s_glCompileShader;
PFNGLGETSHADERIVPROC s_glGetShaderiv;
PFNGLGETSHADERINFOLOGPROC s_glGetShaderInfoLog;
PFNGLSHADERSOURCEPROC s_glShaderSource;

PFNGLCREATEPROGRAMPROC s_glCreateProgram;
PFNGLDELETEPROGRAMPROC s_glDeleteProgram;
PFNGLGETPROGRAMIVPROC s_glGetProgramiv;
PFNGLGETPROGRAMINFOLOGPROC s_glGetProgramInfoLog;
PFNGLLINKPROGRAMPROC s_glLinkProgram;
PFNGLUSEPROGRAMPROC s_glUseProgram;

HGLRC cur_context = nullptr;
HDC app_HDC = nullptr;
GLint s_iMaxTexUnits;

static bool glsl_shader_error = false;

bool LoadOpenGLFunctions()
{
	auto handle = GetModuleHandle(L"opengl32.dll");
#define GAPIOGLFUNC(a,b,c) s_##a = (PFN##a) GetProcAddress(handle, #a); \
	if ( s_##a == nullptr ) \
	{ \
		LOG_MSG( "GL:LoadFunctions: GetProcAddress FAIL("#a")"LOG_CR ); \
		return false; \
	}
#include "GAPI_ogl.tbl"
#undef GAPIOGLFUNC

	s_glActiveTextureARB = (PFNGLACTIVETEXTUREARBPROC)s_wglGetProcAddress("glActiveTextureARB");
	
	s_glBindBuffer = (PFNGLBINDBUFFERPROC)s_wglGetProcAddress("glBindBuffer");
	s_glGenBuffers = (PFNGLGENBUFFERSPROC)s_wglGetProcAddress("glGenBuffers");
	s_glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)s_wglGetProcAddress("glDeleteBuffers");
	s_glBufferData = (PFNGLBUFFERDATAPROC)s_wglGetProcAddress("glBufferData");
	s_glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)s_wglGetProcAddress("glVertexAttribPointer");
	s_glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)s_wglGetProcAddress("glEnableVertexAttribArray");
	s_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)s_wglGetProcAddress("glBindVertexArray");
	s_glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)s_wglGetProcAddress("glGenVertexArrays");
	s_glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)s_wglGetProcAddress("glDeleteVertexArrays");

	s_glCreateShader = (PFNGLCREATESHADERPROC)s_wglGetProcAddress("glCreateShader");
	s_glDeleteShader = (PFNGLDELETESHADERPROC)s_wglGetProcAddress("glDeleteShader");
	s_glAttachShader = (PFNGLATTACHSHADERPROC)s_wglGetProcAddress("glAttachShader");
	s_glBindAttribLocation = (PFNGLBINDATTRIBLOCATIONPROC)s_wglGetProcAddress("glBindAttribLocation");
	s_glCompileShader = (PFNGLCOMPILESHADERPROC)s_wglGetProcAddress("glCompileShader");
	s_glGetShaderiv = (PFNGLGETSHADERIVPROC)s_wglGetProcAddress("glGetShaderiv");
	s_glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)s_wglGetProcAddress("glGetShaderInfoLog");
	s_glShaderSource = (PFNGLSHADERSOURCEPROC)s_wglGetProcAddress("glShaderSource");

	s_glCreateProgram = (PFNGLCREATEPROGRAMPROC)s_wglGetProcAddress("glCreateProgram");
	s_glDeleteProgram = (PFNGLDELETEPROGRAMPROC)s_wglGetProcAddress("glDeleteProgram");
	s_glGetProgramiv = (PFNGLGETPROGRAMIVPROC)s_wglGetProcAddress("glGetProgramiv");
	s_glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)s_wglGetProcAddress("glGetProgramInfoLog");
	s_glLinkProgram = (PFNGLLINKPROGRAMPROC)s_wglGetProcAddress("glLinkProgram");
	s_glUseProgram = (PFNGLUSEPROGRAMPROC)s_wglGetProcAddress("glUseProgram");


	if (s_glActiveTextureARB)
	{
		s_glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &s_iMaxTexUnits);
		LOG_MSG("OpenGL Maximum texture units: %d" LOG_CR, s_iMaxTexUnits);
	}
	else
		LOG_WARN("OpenGL Multi texturing not supported" LOG_CR);

	return true;
}

const GLenum s_TextureUnitNums[32] = 
{
	GL_TEXTURE0_ARB,  GL_TEXTURE1_ARB,  GL_TEXTURE2_ARB,  GL_TEXTURE3_ARB,
	GL_TEXTURE4_ARB,  GL_TEXTURE5_ARB,  GL_TEXTURE6_ARB,  GL_TEXTURE7_ARB,
	GL_TEXTURE8_ARB,  GL_TEXTURE9_ARB,  GL_TEXTURE10_ARB, GL_TEXTURE11_ARB,
	GL_TEXTURE12_ARB, GL_TEXTURE13_ARB, GL_TEXTURE14_ARB, GL_TEXTURE15_ARB,
	GL_TEXTURE16_ARB, GL_TEXTURE17_ARB, GL_TEXTURE18_ARB, GL_TEXTURE19_ARB,
	GL_TEXTURE20_ARB, GL_TEXTURE21_ARB, GL_TEXTURE22_ARB, GL_TEXTURE23_ARB,
	GL_TEXTURE24_ARB, GL_TEXTURE25_ARB, GL_TEXTURE26_ARB, GL_TEXTURE27_ARB,
	GL_TEXTURE28_ARB, GL_TEXTURE29_ARB, GL_TEXTURE30_ARB, GL_TEXTURE31_ARB,
};
OpenGLRenderer::OpenGLRenderer( void ): m_uTexID(0)
{
	for ( int i = 0; i < INDICATE_NONE; i++ )
		m_uTexIDIndicators[i] = 0;
}

OpenGLRenderer::~OpenGLRenderer( void )
{

}

bool OpenGLRenderer::InitRenderer( IndicatorManager &manager )
{
	s_glGenTextures( INDICATE_NONE, m_uTexIDIndicators );

	if (!UpdateIndicatorImages(manager, true))
		return false;
	
	s_glDisable( GL_ALPHA_TEST );
	s_glEnable( GL_BLEND );
	s_glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

	return true;

}

bool OpenGLRenderer::UpdateIndicatorImages(IndicatorManager &manager, bool force)
{
	using namespace Gdiplus;

	for ( int i = 0; i < INDICATE_NONE; i++ )
	{
		auto ev = static_cast<IndicatorEvent>(i);
		if (!force && !manager.ImageUpdated(ev))
			continue;

		auto bmp = manager.GetImage( i );
		BitmapData data;
	
		if ( !bmp )
		{
			LOG_MSG("UpdateIndicatorImages: invalid bitmap!" LOG_CR);
			return false;
		}

		if ( bmp->LockBits(&Rect( 0, 0, bmp->GetWidth( ), bmp->GetHeight( ) ), ImageLockModeRead, PixelFormat32bppARGB, &data ) != Status::Ok )
		{
			LOG_MSG("UpdateIndicatorImages: bitmap[%d] data lock failed!" LOG_CR, i);
			return false;
		}

		LOG_MSG("UpdateIndicatorImages: locked bitmap %d, image stride is %d for size %dx%d" LOG_CR, i, data.Stride, bmp->GetWidth(), bmp->GetHeight());

		m_sIndicatorSize[i].cx = bmp->GetWidth( );
		m_sIndicatorSize[i].cy = bmp->GetHeight( );

		s_glBindTexture( GL_TEXTURE_2D, m_uTexIDIndicators[i] );
		s_glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		s_glPixelStorei( GL_UNPACK_ROW_LENGTH, bmp->GetWidth( ) );
		s_glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
		s_glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
		s_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, bmp->GetWidth( ), bmp->GetHeight( ), 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, data.Scan0 );
		int err = s_glGetError( );
		if ( err )
			LOG_WARN("UpdateIndicatorImages: unable to update OpenGL texture (%d)" LOG_CR, err);

		s_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		s_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );

		bmp->UnlockBits( &data );

		manager.ResetImageUpdated(ev);
	}

	return true;
}

void OpenGLRenderer::FreeRenderer( void )
{
	if ( *m_uTexIDIndicators ) // if first indicator texture is set, the rest should be
		s_glDeleteTextures( INDICATE_NONE, m_uTexIDIndicators );
}

void OpenGLRenderer::DrawIndicator( TAKSI_INDICATE_TYPE eIndicate )
{
	s_glPushAttrib( GL_ALL_ATTRIB_BITS );

#define INDIC_SIZE 32
	s_glViewport( 0, g_Proc.m_Stats.m_SizeWnd.cy-INDIC_SIZE, INDIC_SIZE, INDIC_SIZE );
	s_glMatrixMode( GL_PROJECTION );
	s_glPushMatrix( );
	s_glLoadIdentity( );
	s_glOrtho( 0.0, INDIC_SIZE, 0.0, INDIC_SIZE, -1.0, 1.0 );
	s_glMatrixMode( GL_MODELVIEW );
	s_glPushMatrix( );
	s_glLoadIdentity( );

	s_glDisable( GL_LIGHTING );
	s_glDisable( GL_TEXTURE_1D );
	if ( s_glActiveTextureARB )
	{
		for ( int i=0; i<s_iMaxTexUnits; i++ )
		{
			//DEBUG_TRACE(("disabling GL_TEXTURE%d_ARB..." LOG_CR, i));
			s_glActiveTextureARB( s_TextureUnitNums[i] );
			s_glDisable( GL_TEXTURE_2D );
		}
	}
	else
	{
		s_glDisable( GL_TEXTURE_2D );
	}
	s_glDisable( GL_DEPTH_TEST );
	s_glShadeModel( GL_FLAT );

	// set appropriate color for the indicator
	// CTaksiGAPIBase::sm_IndColors
	switch ( eIndicate )
	{
	case TAKSI_INDICATE_Idle:
		s_glColor3f( 0.53f, 0.53f, 0.53f );
		break;
	case TAKSI_INDICATE_Hooking:
		s_glColor3f( 0.26f, 0.53f, 1.0f ); // BLUE: system wide hooking mode
		break;
	case TAKSI_INDICATE_Ready:
		s_glColor3f( 0.53f, 1.0f, 0.0f ); // GREEN: normal mode
		break;
	case TAKSI_INDICATE_Recording:
		s_glColor3f( 1.0f, 0.26f, 0.0f ); // RED: recording mode
		break;
	case TAKSI_INDICATE_RecordPaused:
		s_glColor3f( 0.26f, 0.26f, 0.26f ); // GRAY: paused mode
		break;
	default:
		DbgRaiseAssertionFailure();
		return;
	}

	s_glBegin( GL_POLYGON );
		s_glVertex2i( INDICATOR_X, INDICATOR_Y );
		s_glVertex2i( INDICATOR_X, INDICATOR_Y + INDICATOR_Height );
		s_glVertex2i( INDICATOR_X + INDICATOR_Width, INDICATOR_Y + INDICATOR_Height );
		s_glVertex2i( INDICATOR_X + INDICATOR_Width, INDICATOR_Y );
	s_glEnd( );

	// black outline
	s_glColor3f( 0.0, 0.0, 0.0 );
	s_glBegin( GL_LINE_LOOP );
		s_glVertex2i( INDICATOR_X, INDICATOR_Y );
		s_glVertex2i( INDICATOR_X, INDICATOR_Y + INDICATOR_Height );
		s_glVertex2i( INDICATOR_X + INDICATOR_Width, INDICATOR_Y + INDICATOR_Height );
		s_glVertex2i( INDICATOR_X + INDICATOR_Width, INDICATOR_Y );
	s_glEnd();

	s_glMatrixMode( GL_PROJECTION );
	s_glPopMatrix( );
	s_glMatrixMode( GL_MODELVIEW );
	s_glPopMatrix( );

	s_glPopAttrib( );
}

static GLuint vshader = 0, fshader = 0, glprogram = 0;
static std::array<GLuint, 3> gl_buffers = { 0 };
static GLuint varray = 0;
const GLchar *vertexShader =	"#version 330\n\n" \
								"layout(location = 0) in vec3 vpos;\n" \
								"layout(location = 1) in vec4 vcolor;\n" \
								"layout(location = 2) in vec2 texc;\n" \
								"\n" \
								"out vec2 texcoord;\n" \
								"out vec4 color;\n\n" \
								"void main() {\n" \
									"color = vcolor;\n" \
									"texcoord = texc;" \
									"gl_Position = vec4(vpos, 1.0);\n" \
								"}\n";

const GLchar *fragmentShader =	"#version 330\n\n" \
								"uniform sampler2D tex;\n" \
								"in vec2 texcoord;\n" \
								"in vec4 color;\n" \
								"out vec4 col_out;\n\n" \
								"void main() {\n" \
									"col_out = texture(tex, texcoord);\n" \
								"}\n";

void RenderOGLTexture(int w, int h, GLuint texture) {
	float x, y;

	x = 1.0f - (((float)w / (float)g_Proc.m_Stats.m_SizeWnd.cx) * 2.0f);
	y = 1.0f - (((float)h / (float)g_Proc.m_Stats.m_SizeWnd.cy) * 2.0f);

	float indicatorPos[] = {
		x,  y,  0.0f,
		x,  1.0f,  0.0f,
		1.0f,  y,  0.0f,
		1.0f,  1.0f,  0.0f,
	};

	float indicatorColor[] = {
		1.0f, 0.0f, 0.0f, 1.0f,
		0.0f, 1.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f, 1.0f,
		1.0f, 0.0f, 1.0f, 1.0f,
	};

	float texCoords[] = {
		0.0f,  1.0f,
		0.0f,  0.0f,
		1.0f,  1.0f,
		1.0f,  0.0f,
	};

	GLint Result = 0, InfoLogLength = 0;

	if (vshader == 0) {
		vshader = s_glCreateShader(GL_VERTEX_SHADER);
		s_glShaderSource(vshader, 1, &vertexShader, NULL);
		s_glCompileShader(vshader);
		s_glGetShaderiv(vshader, GL_COMPILE_STATUS, &Result);
		s_glGetShaderiv(vshader, GL_INFO_LOG_LENGTH, &InfoLogLength);
		if (InfoLogLength > 0) {
			std::vector<char> ErrorMessage(InfoLogLength + 1);
			s_glGetShaderInfoLog(vshader, InfoLogLength, NULL, &ErrorMessage[0]);
			LOG_MSG("Vertex shader compile error: %s", &ErrorMessage[0] LOG_CR);
			glsl_shader_error = true;
		}
	}

	if (fshader == 0) {
		fshader = s_glCreateShader(GL_FRAGMENT_SHADER);
		s_glShaderSource(fshader, 1, &fragmentShader, NULL);
		s_glCompileShader(fshader);
		s_glGetShaderiv(fshader, GL_COMPILE_STATUS, &Result);
		s_glGetShaderiv(fshader, GL_INFO_LOG_LENGTH, &InfoLogLength);
		if (InfoLogLength > 0) {
			std::vector<char> ErrorMessage(InfoLogLength + 1);
			s_glGetShaderInfoLog(fshader, InfoLogLength, NULL, &ErrorMessage[0]);
			LOG_MSG("Fragment shader compile error: %s", &ErrorMessage[0] LOG_CR);
			glsl_shader_error = true;
		}
	}

	if (glprogram == 0) {
		glprogram = s_glCreateProgram();
		s_glAttachShader(glprogram, fshader);
		s_glAttachShader(glprogram, vshader);
		s_glBindAttribLocation(glprogram, 0, "vpos");
		s_glBindAttribLocation(glprogram, 1, "vcolor");
		s_glBindAttribLocation(glprogram, 2, "texc");
		s_glLinkProgram(glprogram);
		s_glGetProgramiv(glprogram, GL_LINK_STATUS, &Result);
		s_glGetProgramiv(glprogram, GL_INFO_LOG_LENGTH, &InfoLogLength);
		if (InfoLogLength > 0) {
			std::vector<char> ErrorMessage(InfoLogLength + 1);
			s_glGetProgramInfoLog(glprogram, InfoLogLength, NULL, &ErrorMessage[0]);
			LOG_MSG("GL Program compile error: %s", &ErrorMessage[0]);
			glsl_shader_error = true;
		}
	}

	if (glsl_shader_error) {
		LOG_MSG("Could not set up OpenGL shaders and/or program. Using glVertex rendering fallback." LOG_CR);
		return;
	}

	if (!gl_buffers[0])
		s_glGenBuffers(gl_buffers.size(), gl_buffers.data());

	GLuint points = gl_buffers[0], color = gl_buffers[1], texcoords = gl_buffers[2];

	s_glBindBuffer(GL_ARRAY_BUFFER, points);
	s_glBufferData(GL_ARRAY_BUFFER, 12 * sizeof(float), indicatorPos, GL_STATIC_DRAW);
	s_glBindBuffer(GL_ARRAY_BUFFER, color);
	s_glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), indicatorColor, GL_STATIC_DRAW);
	s_glBindBuffer(GL_ARRAY_BUFFER, texcoords);
	s_glBufferData(GL_ARRAY_BUFFER, 8 * sizeof(float), texCoords, GL_STATIC_DRAW);

	if (!varray)
		s_glGenVertexArrays(1, &varray);
	s_glBindVertexArray(varray);

	s_glBindBuffer(GL_ARRAY_BUFFER, points);
	s_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);
	s_glBindBuffer(GL_ARRAY_BUFFER, color);
	s_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	s_glBindBuffer(GL_ARRAY_BUFFER, texcoords);
	s_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, NULL);

	s_glEnableVertexAttribArray(0);
	s_glEnableVertexAttribArray(1);
	s_glEnableVertexAttribArray(2);

	s_glUseProgram(glprogram);
	s_glBindVertexArray(varray);
	s_glBindTexture(GL_TEXTURE_2D, texture);
	s_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void OpenGLRenderer::DrawNewIndicator( IndicatorEvent eIndicatorEvent, BYTE alpha )
{
	int err = 0;
	// opengl positioning is still a fucking nightmare, these might not even work properly with all indicators yet
	// the y coordinate has to be upside down because opengl's origin point is the bottom-left corner of the screen
	// so we start from bottom left and wind around clockwise (top left, top right, bottom right) to draw our textured quad
	int w = m_sIndicatorSize[eIndicatorEvent].cx; 
	int h = m_sIndicatorSize[eIndicatorEvent].cy;
	int x = g_Proc.m_Stats.m_SizeWnd.cx - w;
	int y = g_Proc.m_Stats.m_SizeWnd.cy - m_sIndicatorSize[eIndicatorEvent].cy;

	if (w == 0 || h == 0)
		return;

	s_glPushAttrib( GL_ALL_ATTRIB_BITS );

	s_glMatrixMode( GL_PROJECTION );
	s_glPushMatrix( );
	s_glLoadIdentity( );
	s_glViewport( 0, 0, g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy );
	s_glOrtho( 0, g_Proc.m_Stats.m_SizeWnd.cx, 0, g_Proc.m_Stats.m_SizeWnd.cy, -1.0, 1.0 );
	s_glMatrixMode( GL_MODELVIEW );
	s_glPushMatrix( );
	s_glLoadIdentity( );

	s_glDisable( GL_LIGHTING );
	s_glDisable( GL_TEXTURE_1D );
	if ( s_glActiveTextureARB )
	{
		for ( int i = 1; i < s_iMaxTexUnits; i++ ) // leave unit 0 alone but disable others
		{
			s_glActiveTextureARB( s_TextureUnitNums[i] );
			s_glDisable( GL_TEXTURE_2D );
		}
		s_glActiveTextureARB( GL_TEXTURE0_ARB );
	}
	
	s_glEnable( GL_TEXTURE_2D );
	
	s_glDisable( GL_DEPTH_TEST );
	s_glDisable( GL_CULL_FACE );
	s_glShadeModel( GL_SMOOTH );

	s_glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

	s_glColor4ub( 255, 255, 255, alpha );

	if (!glsl_shader_error)
		RenderOGLTexture(w, h, m_uTexIDIndicators[eIndicatorEvent]);
	if (glsl_shader_error) {
		h = g_Proc.m_Stats.m_SizeWnd.cy; // taller images are meant to hang down from the top of the screen

		s_glBindTexture(GL_TEXTURE_2D, m_uTexIDIndicators[eIndicatorEvent]);
		err = s_glGetError();
		if (err)
			LOG_MSG("DrawNewIndicator: glBindTexture returned error: %d" LOG_CR, err);

		//s_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		//s_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );

		s_glColor4ub(255, 255, 255, alpha);

		// draw things. fuck opengl and it's bottom-left origin (so vertical shit is all flipped)
		s_glBegin(GL_QUADS);
		s_glTexCoord2f(0.0, 0.0);
		s_glVertex2i(x, h);

		s_glTexCoord2f(0.0, 1.0);
		s_glVertex2i(x, y);

		s_glTexCoord2f(1.0, 1.0);
		s_glVertex2i(x + w, y);

		s_glTexCoord2f(1.0, 0.0);
		s_glVertex2i(x + w, h);
		s_glEnd();
	}

	err = s_glGetError( );
	if ( err )
		LOG_MSG( "DrawNewIndicator: OpenGL draw returned error: %d" LOG_CR, err );

	s_glMatrixMode( GL_MODELVIEW );
	s_glPopMatrix( );

	s_glMatrixMode( GL_PROJECTION );
	s_glPopMatrix( );

	s_glPopAttrib ();
}

void OpenGLRenderer::DrawOverlay( void )
{
	/*
	int err = 0;
	//int w = 480, h = 800;
	//int sw = g_Proc.m_Stats.m_SizeWnd.cx, sh = g_Proc.m_Stats.m_SizeWnd.cy;
	int w = g_Proc.m_Stats.m_SizeWnd.cx, h = g_Proc.m_Stats.m_SizeWnd.cy;

	s_glPushAttrib(GL_ALL_ATTRIB_BITS);

	s_glMatrixMode(GL_PROJECTION);
	s_glPushMatrix();
	s_glLoadIdentity();
	s_glViewport(0, 0, w, h);
	s_glOrtho(0.0, w, h, 0.0, -1.0, 1.0);
	s_glMatrixMode(GL_MODELVIEW);
	s_glPushMatrix();
	s_glLoadIdentity();

	s_glDisable(GL_LIGHTING);
	s_glDisable(GL_TEXTURE_1D);
	if (s_glActiveTextureARB)
	{
		for (int i = 1; i < s_iMaxTexUnits; i++ ) // leave unit 0 alone but disable others
		{
			s_glActiveTextureARB( s_TextureUnitNums[i] );
			s_glDisable(GL_TEXTURE_2D);
		}
		s_glActiveTextureARB( GL_TEXTURE0_ARB );
	}
	
	s_glEnable(GL_TEXTURE_2D);
	
	s_glDisable(GL_DEPTH_TEST);
	s_glDisable(GL_CULL_FACE);
	s_glShadeModel(GL_SMOOTH);

	s_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	s_glBindTexture(GL_TEXTURE_2D, m_uTexID);
	err = s_glGetError( );
	if ( err )
		LOG_MSG("CTaksiGAPI_OGL::IGRenderFrame (bind): OpenGL returned error: %d" LOG_CR, err);
	
	// update texture from memory buffer
	s_glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	s_glPixelStorei( GL_UNPACK_ROW_LENGTH, w );
	s_glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
	s_glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
	s_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, m_buffer );
	err = s_glGetError( );
	if ( err )
		LOG_WARN("CTaksiGAPI_OGL::IGRenderFrame: unable to update OpenGL texture (%d)" LOG_CR, err);

	s_glColor4f(1.0f, 1.0f, 1.0f, 0.8f);

	// draw things
	s_glBegin(GL_QUADS);
		s_glTexCoord2f(0.0, 0.0);
		s_glVertex2f(0, 0);
		
		s_glTexCoord2f(0.0, 1.0);
		s_glVertex2f(0, h);
		
		s_glTexCoord2f(1.0, 1.0);
		s_glVertex2f(w, h);
		
		s_glTexCoord2f(1.0, 0.0);
		s_glVertex2f(w, 0);
	s_glEnd();
	err = s_glGetError( );
	if ( err )
		LOG_MSG("CTaksiGAPI_OGL::IGRenderFrame (draw): OpenGL returned error: %d" LOG_CR, err);

	s_glMatrixMode(GL_MODELVIEW);
	s_glPopMatrix();

	s_glMatrixMode(GL_PROJECTION);
	s_glPopMatrix();

	s_glPopAttrib();
	*/
}

static void get_window_size(HDC hdc, LONG *cx, LONG *cy)
{
	HWND hwnd = WindowFromDC(hdc);
	RECT rc = { 0 };

	GetClientRect(hwnd, &rc);
	*cx = rc.right;
	*cy = rc.bottom;
}

static OpenGLRenderer render;
static bool initialized = false;
static bool framebuffer_server_started = false;
static HGLRC render_context = nullptr;

static TextureBufferingHelper<GLuint> overlay_textures[OVERLAY_COUNT];
static bool overlay_tex_initialized = false;

static bool in_free = false;
void overlay_gl_free()
{
	if (in_free || !initialized)
		return;

	auto dc_valid = !!WindowFromDC(app_HDC);

	in_free = true;

	if (dc_valid) {
		s_wglMakeCurrent(app_HDC, render_context); // Switch to the overlay context for these operations.

		for (uint32_t a = OVERLAY_HIGHLIGHTER; a < OVERLAY_COUNT; a++) {
			overlay_textures[a].Reset([&](GLuint &tex)
			{
				if (!tex)
					return;

				s_glDeleteTextures(1, &tex);
				tex = 0;
			});
		}

		if (vshader && s_glDeleteShader)
			s_glDeleteShader(vshader);
		if (fshader && s_glDeleteShader)
			s_glDeleteShader(fshader);
		if (glprogram && s_glDeleteProgram)
			s_glDeleteProgram(glprogram);

		if (gl_buffers[0] && s_glDeleteBuffers)
			s_glDeleteBuffers(gl_buffers.size(), gl_buffers.data());

		if (varray && s_glDeleteVertexArrays)
			s_glDeleteVertexArrays(1, &varray);

		render.FreeRenderer();
	}

	if (render_context && dc_valid)
		s_wglDeleteContext(render_context);

	render_context = nullptr;

	overlay_tex_initialized = false;
	framebuffer_server_started = false;

	initialized = false;
	in_free = false;
	glsl_shader_error = false;

	vshader = 0;
	fshader = 0;
	glprogram = 0;

	for (auto &buffer : gl_buffers)
		buffer = 0;

	varray = 0;

	if (dc_valid && cur_context)
		s_wglMakeCurrent(app_HDC, cur_context); // And then switch back the game's context.
}

static void update_overlay()
{
	s_glPushAttrib(GL_ALL_ATTRIB_BITS);

	if (indicatorManager.updateTextures && render.UpdateIndicatorImages(indicatorManager))
		indicatorManager.updateTextures = false;

	if (!overlay_tex_initialized)
	{
		try {
			for (uint32_t a = OVERLAY_HIGHLIGHTER; a < OVERLAY_COUNT; a++) {
				overlay_textures[a].Apply([&](GLuint &tex)
				{
					s_glGenTextures(1, &tex);

					s_glBindTexture(GL_TEXTURE_2D, tex);
					s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					auto err = s_glGetError();
					if (err)
						throw err;
				});
			}
		}
		catch (GLenum err)
		{
			hlog("update_overlay: unable to initialize OpenGL texture (%d)\n", err);
			return;
		}

		overlay_tex_initialized = true;
	}

	for (size_t i = OVERLAY_HIGHLIGHTER; i < OVERLAY_COUNT; i++)
		overlay_textures[i].Buffer([&](GLuint &tex)
		{
			auto vec = ReadNewFramebuffer(static_cast<ActiveOverlay>(i));
			if (!vec || vec->size() != g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4)
				return false;

			s_glBindTexture(GL_TEXTURE_2D, tex);
			s_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			s_glPixelStorei(GL_UNPACK_ROW_LENGTH, g_Proc.m_Stats.m_SizeWnd.cx);
			s_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
			s_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
			s_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_Proc.m_Stats.m_SizeWnd.cx,
				g_Proc.m_Stats.m_SizeWnd.cy, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, vec->data());
			auto err = s_glGetError();
			if (err) {
				LOG_WARN("update_overlay: unable to update OpenGL texture %d (%d)" LOG_CR, i, err);
				return false;
			}

			return true;
		});
	s_glPopAttrib();
}

static bool show_browser_tex_(const ActiveOverlay &active_overlay)
{
	return overlay_textures[active_overlay].Draw([&](GLuint &tex)
	{
		int err = 0;
		// opengl positioning is still a fucking nightmare, these might not even work properly with all indicators yet
		// the y coordinate has to be upside down because opengl's origin point is the bottom-left corner of the screen
		// so we start from bottom left and wind around clockwise (top left, top right, bottom right) to draw our textured quad
		int w = g_Proc.m_Stats.m_SizeWnd.cx;
		int h = g_Proc.m_Stats.m_SizeWnd.cy; // taller images are meant to hang down from the top of the screen
		int x = 0;
		int y = 0;

		s_glPushAttrib(GL_ALL_ATTRIB_BITS);

		s_glMatrixMode(GL_PROJECTION);
		s_glPushMatrix();
		s_glLoadIdentity();
		s_glViewport(0, 0, g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy);
		s_glOrtho(0, g_Proc.m_Stats.m_SizeWnd.cx, 0, g_Proc.m_Stats.m_SizeWnd.cy, -1.0, 1.0);
		s_glMatrixMode(GL_MODELVIEW);
		s_glPushMatrix();
		s_glLoadIdentity();

		s_glDisable(GL_LIGHTING);
		s_glDisable(GL_TEXTURE_1D);
		
		if(!glsl_shader_error)
			RenderOGLTexture(w, h, tex);
		if (glsl_shader_error) {
			if (s_glActiveTextureARB)
			{
				for (int i = 1; i < s_iMaxTexUnits; i++) // leave unit 0 alone but disable others
				{
					s_glActiveTextureARB(s_TextureUnitNums[i]);
					s_glDisable(GL_TEXTURE_2D);
				}
				s_glActiveTextureARB(GL_TEXTURE0_ARB);
			}

			s_glEnable(GL_TEXTURE_2D);

			s_glDisable(GL_DEPTH_TEST);
			s_glDisable(GL_CULL_FACE);
			s_glShadeModel(GL_SMOOTH);

			s_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

			s_glBindTexture(GL_TEXTURE_2D, tex);
			err = s_glGetError();
			if (err)
				LOG_MSG("show_browser_tex_: glBindTexture returned error: %d" LOG_CR, err);

			s_glColor4ub(255, 255, 255, 255);

			// draw things. fuck opengl and it's bottom-left origin (so vertical shit is all flipped)
			s_glBegin(GL_QUADS);
			s_glTexCoord2f(0.0, 0.0);
			s_glVertex2i(x, h);

			s_glTexCoord2f(0.0, 1.0);
			s_glVertex2i(x, y);

			s_glTexCoord2f(1.0, 1.0);
			s_glVertex2i(x + w, y);

			s_glTexCoord2f(1.0, 0.0);
			s_glVertex2i(x + w, h);
			s_glEnd();
		}

		err = s_glGetError();
		if (err)
			LOG_MSG("show_browser_tex_: OpenGL draw returned error: %d" LOG_CR, err);

		s_glMatrixMode(GL_MODELVIEW);
		s_glPopMatrix();

		s_glMatrixMode(GL_PROJECTION);
		s_glPopMatrix();

		s_glPopAttrib();

		return true;
	});
}

static bool show_browser_tex(const ActiveOverlay &active_overlay = ::active_overlay)
{
	s_glPushAttrib(GL_ALL_ATTRIB_BITS);
	bool res = show_browser_tex_(active_overlay);
	s_glPopAttrib();
	return res;
}

C_EXPORT void overlay_draw_gl(HDC hdc)
{
	if (!s_wglCreateContext)
		LoadOpenGLFunctions();

	if (!WindowFromDC(app_HDC)) {
		app_HDC = nullptr;
		overlay_gl_free();
	}

	if (!render_context)
		render_context = s_wglCreateContext(hdc);

	cur_context = s_wglGetCurrentContext();
	app_HDC = hdc;

	s_wglMakeCurrent(hdc, render_context);
	DEFER {
		s_wglMakeCurrent(hdc, cur_context);
	};

	if (!initialized)
		initialized = render.InitRenderer(indicatorManager);

	get_window_size(hdc, &g_Proc.m_Stats.m_SizeWnd.cx, &g_Proc.m_Stats.m_SizeWnd.cy);

	if (!framebuffer_server_started) {
		StartFramebufferServer();
		framebuffer_server_started = true;
	}

	HandleInputHook(WindowFromDC(hdc));

	//render.DrawIndicator(TAKSI_INDICATE_Recording);

	update_overlay();

	if (g_bBrowserShowing && show_browser_tex())
		return;

	ShowCurrentIndicator([](IndicatorEvent indicator, BYTE alpha)
	{
		show_browser_tex(OVERLAY_NOTIFICATIONS);
		render.DrawNewIndicator(indicator, alpha);
	});
}
