// [GAPI_dx9_render.h 2014-07-04 abright]
// new home for any dx9 rendering code

#ifndef GAPI_DX9_RENDER_NASTY
#define GAPI_DX9_RENDER_NASTY

#include "IRefPtr.h"

#include "TextureBufferingHelper.hpp"

class DX9Renderer
{
private:
	IDirect3DDevice9 *m_pDevice;

	IRefPtr<IDirect3DVertexBuffer9> m_pVBSquareIndicator;
	IRefPtr<IDirect3DVertexBuffer9> m_pVBSquareBorder;
	IRefPtr<IDirect3DVertexBuffer9> m_pVBOverlay;
	IRefPtr<IDirect3DVertexBuffer9> m_pVBNotification;

	IRefPtr<IDirect3DStateBlock9> m_pCurrentRenderState; // render state of whatever we hooked
	IRefPtr<IDirect3DStateBlock9> m_pSolidRenderState; // our render state for solid drawing
	IRefPtr<IDirect3DStateBlock9> m_pTexturedRenderState; // our render state for textured drawing

	IRefPtr<IDirect3DTexture9> m_pIndicatorTexture[INDICATE_NONE]; // indicator images
	using OverlayTexture_t = IRefPtr<IDirect3DTexture9>;
	TextureBufferingHelper<OverlayTexture_t> overlay_textures[OVERLAY_COUNT];

	void UpdateSquareBorderVB( IDirect3DVertexBuffer9 *pVB, int x, int y, int w, int h, DWORD color ); // update the square indicator border vertex buffer (it's bigger than the solid/textured quad ones)
	void UpdateVB( IDirect3DVertexBuffer9 *pVB, int x, int y, int w, int h, DWORD color ); // update given vertex buffer position/size/color
	void SetupRenderState( IDirect3DStateBlock9 **pStateBlock, DWORD vp_width, DWORD vp_height, bool textured ); // setup rendering state blocks. false = solid, true = textured
	void InitIndicatorTextures( IndicatorManager &manager ); // helper to create indicator textures from the manager

	template <typename Fun>
	bool RenderTex(Fun &&f);
public:
	DX9Renderer( void );
	~DX9Renderer( void );

	// init all the d3d objects we'll need (including indicator textures)
	bool InitRenderer( IDirect3DDevice9 *pDevice, IndicatorManager &manager );
	// release/free everything. could be unhooking or a device reset.
	void FreeRenderer( void );

	// draw the old indicator
	void DrawIndicator( TAKSI_INDICATE_TYPE eIndicate );
	// draw the new indicator images
	void DrawNewIndicator( IndicatorEvent eIndicatorEvent, DWORD color );
	// draw overlay
	bool DrawOverlay( void );
	void UpdateOverlay();
};

#endif