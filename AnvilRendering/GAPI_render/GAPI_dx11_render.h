// [GAPI_dx11_render.h 2013-09-05 abright]
// dx11 rendering stuff, separated from the capture code because there's so much of it
// indicator drawing will move here at some point in future

#ifndef GAPI_DX11_RENDER_NASTY
#define GAPI_DX11_RENDER_NASTY

#include "IRefPtr.h"

#include "TextureBufferingHelper.hpp"

// Structs for DWORD color to floats and transform to screen coords
struct D3D11COLOR
{
	D3D11COLOR( UINT argb )
	{ 
		a = (float)((argb >> 24) & 255) / 255;
		r = (float)((argb >> 16) & 255) / 255;
		g = (float)((argb >> 8) & 255) / 255;
		b = (float)((argb) & 255) / 255;
	}
	D3D11COLOR( float red, float green, float blue, float alpha )
	{
		r = red;
		g = green;
		b = blue;
		a = alpha;
	}

	float r, g, b, a;
};

struct D3D11POS
{
	D3D11POS( float _x, float _y, float _z ) 
	{
		float vpWidth = ( INDICATOR_X*2 + INDICATOR_Width );
		float vpHeight = ( INDICATOR_Y*2 + INDICATOR_Height );

		x = (_x / (vpWidth/2.0f)) - 1.0f;
		y = -(_y / (vpHeight/2.0f)) + 1.0f;
		z = _z;
		w = 1.0f;
	}
	D3D11POS( float _x, float _y ) 
	{
		float vpWidth = ( g_Proc.m_Stats.m_SizeWnd.cx );
		float vpHeight = ( g_Proc.m_Stats.m_SizeWnd.cy );

		x = (_x / (vpWidth/2.0f)) - 1.0f;
		y = -(_y / (vpHeight/2.0f)) + 1.0f;
		z = 0.0f;
		w = 1.0f;
	}

	float x, y, z, w;
};

struct D3D11TEX
{
	float u, v;
};

struct D3D11Texture
{
	IRefPtr<ID3D11Texture2D> tex;
	IRefPtr<ID3D11ShaderResourceView> res;
};

class DX11Renderer
{
private:
	ID3D11Device *m_pDevice;

	ID3D11RasterizerState *m_pRasterState;
	ID3D11DepthStencilState *m_pDepthState;

	IRefPtr<ID3D11Texture2D> m_pIndicatorTexture[INDICATE_NONE]; // textures for new indicators

	TextureBufferingHelper<D3D11Texture> overlay_textures[OVERLAY_COUNT];

	IRefPtr<ID3D11Buffer> m_pVBSquareIndicator;
	IRefPtr<ID3D11Buffer> m_pVBSquareBorder;
	IRefPtr<ID3D11Buffer> m_pVBNotification;
	IRefPtr<ID3D11Buffer> m_pVBOverlay;
	IRefPtr<ID3D11InputLayout> m_pVertexLayout;

	IRefPtr<ID3D11VertexShader> m_pVertexShader;
	IRefPtr<ID3D11PixelShader> m_pPixelShader; // texture-mapped pixel shader
	IRefPtr<ID3D11PixelShader> m_pPixelShaderSolid; // solid pixel shader
	
	IRefPtr<ID3D11ShaderResourceView> m_pResViewNotification[INDICATE_NONE]; // resource view for each indicator texture
	IRefPtr<ID3D11SamplerState> m_pSamplerState;
	//IRefPtr<ID3D11RenderTargetView> m_pRenderTargetView; // render target view of backbuffer if we don't have one
	IRefPtr<ID3D11BlendState> m_pBlendStateTextured; // blend state for texture drawing

	void InitIndicatorTextures( IndicatorManager &manager ); // helper to create indicator textures from the manager
	void UpdateNotificationVB( IndicatorEvent eIndicatorEvent, BYTE alpha ); // helper to update the indicator vertex buffer
	void UpdateSquareIndicatorVB( TAKSI_INDICATE_TYPE eIndicate ); // helper to update the indicator vertex buffer
	void UpdateOverlayVB(ID3D11Texture2D *tex);
	HRESULT CreateIndicatorVB( void );
public:
	DX11Renderer( ID3D11Device *pDevice );
	~DX11Renderer( void );

	// set up everything we need to resize from one texture to another. pOutputTexture should be 
	bool InitRenderer( IndicatorManager &manager );
	void FreeRenderer( void );

	// draw the new indicator
	void DrawNewIndicator( IDXGISwapChain *pSwapChain, IndicatorEvent eIndicatorEvent, BYTE alpha );
	// draw the old indicator
	void DrawIndicator( IDXGISwapChain *pSwapChain, TAKSI_INDICATE_TYPE eIndicate );

	// draw overlay
	bool DrawOverlay(IDXGISwapChain *pSwapChain);
	void UpdateOverlay();
};

#endif // GAPI_DX11_RENDER_NASTY