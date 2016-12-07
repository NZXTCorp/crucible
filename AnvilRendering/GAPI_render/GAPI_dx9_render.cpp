// [GAPI_dx9_render.cpp 2014-07-04 abright]
// new home for any dx9 rendering code

#include "stdafx.h"
#include "../AnvilRendering.h"
//#include "TaksiDll.h"
//#include "GAPI_Base.h"

#include <d3d9types.h>
#include <d3d9.h>

#include "CImageGDIP.h"
#include "NewIndicator.h"

//#include "GAPI_dx9.h"
#include "GAPI_dx9_render.h"

#include <mutex>

struct NEWVERTEX 
{ 
#define D3DFVF_NEWVERTEX (D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1)
	FLOAT x,y,z,w;
	D3DCOLOR color;
	FLOAT tu, tv;
};

DX9Renderer::DX9Renderer( void )
{

}

DX9Renderer::~DX9Renderer( void )
{

}

bool DX9Renderer::InitRenderer( IDirect3DDevice9 *pDevice, IndicatorManager &manager )
{
// helper to bail on failed d3d calls so we don't have like 200 lines of the same shit	
#define CHEK(hr, a) if ( FAILED(hr) ) { LOG_MSG( "InitRenderer: %s failed with result 0x%x" LOG_CR, a, hr ); return false; }
	
	LOG_MSG( "InitRenderer: initialising renderer" LOG_CR );

	m_pDevice = pDevice;
	if ( !m_pDevice )
	{
		LOG_MSG( "InitRenderer: no device!" LOG_CR );
		return false;
	}

	// check if we've already been initialised
	if ( m_pVBSquareIndicator.IsValidRefObj( ) && m_pCurrentRenderState.IsValidRefObj( ))
	{
		LOG_MSG( "InitRenderer: ignoring as we're already initialised!" LOG_CR );
		return true;
	}

	int width = g_Proc.m_Stats.m_SizeWnd.cx;
	int height = g_Proc.m_Stats.m_SizeWnd.cy;

	// create vertex buffers
	UINT uVBSize = sizeof(NEWVERTEX) * 4;
	HRESULT hRes = pDevice->CreateVertexBuffer( uVBSize, D3DUSAGE_WRITEONLY, D3DFVF_NEWVERTEX, D3DPOOL_DEFAULT, IREF_GETPPTR(m_pVBSquareIndicator, IDirect3DVertexBuffer9), NULL );
	CHEK(hRes, "CreateVertexBuffer 1");

	hRes = pDevice->CreateVertexBuffer( uVBSize + sizeof(NEWVERTEX), D3DUSAGE_WRITEONLY, D3DFVF_NEWVERTEX, D3DPOOL_DEFAULT, IREF_GETPPTR(m_pVBSquareBorder, IDirect3DVertexBuffer9), NULL );
	CHEK(hRes, "CreateVertexBuffer 2");
	
	hRes = pDevice->CreateVertexBuffer( uVBSize, D3DUSAGE_WRITEONLY, D3DFVF_NEWVERTEX, D3DPOOL_DEFAULT, IREF_GETPPTR(m_pVBOverlay, IDirect3DVertexBuffer9), NULL );
	CHEK(hRes, "CreateVertexBuffer 3");
	
	hRes = pDevice->CreateVertexBuffer( uVBSize, D3DUSAGE_WRITEONLY, D3DFVF_NEWVERTEX, D3DPOOL_DEFAULT, IREF_GETPPTR(m_pVBNotification, IDirect3DVertexBuffer9), NULL );
	CHEK(hRes, "CreateVertexBuffer 4");

	// create render states
	hRes = m_pDevice->CreateStateBlock( D3DSBT_ALL, IREF_GETPPTR(m_pCurrentRenderState, IDirect3DStateBlock9) );
	CHEK(hRes, "CreateStateBlock current");
	SetupRenderState( IREF_GETPPTR(m_pSolidRenderState, IDirect3DStateBlock9), INDICATOR_X * 2 + INDICATOR_Width, INDICATOR_Y * 2 + INDICATOR_Height, false );
	SetupRenderState( IREF_GETPPTR(m_pTexturedRenderState, IDirect3DStateBlock9), width, height, true );

	// create textures
	for (uint32_t a = OVERLAY_HIGHLIGHTER; a < OVERLAY_COUNT; a++) {
		overlay_textures[a].Apply([&](OverlayTexture_t &tex)
		{
			m_pDevice->CreateTexture(g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, IREF_GETPPTR(tex, IDirect3DTexture9), NULL);
		});
	}
	InitIndicatorTextures(manager);

	// fill buffers. most will be updated between frames anyway.
	UpdateVB( m_pVBSquareIndicator, INDICATOR_X, INDICATOR_Y, INDICATOR_Width, INDICATOR_Height, 0xFF000000 );
	UpdateSquareBorderVB( m_pVBSquareBorder, INDICATOR_X, INDICATOR_Y, INDICATOR_Width, INDICATOR_Height, 0xFF000000 );
	UpdateVB( m_pVBOverlay, 0, 0, width, height, 0xFFFFFFFF );
	UpdateVB( m_pVBNotification, 0, 0, width, height, 0xFF000000 ); // just fill with dummy values, it's gonna be overwritten anyway

	return true;
}

void DX9Renderer::FreeRenderer( void )
{
#define REK(p) if (p) { p->Release( ); p = nullptr; }
	REK(m_pVBSquareIndicator);
	REK(m_pVBSquareBorder);
	REK(m_pVBOverlay);
	REK(m_pVBNotification);

	REK(m_pCurrentRenderState);
	REK(m_pSolidRenderState);
	REK(m_pTexturedRenderState);

	for (uint32_t a = OVERLAY_HIGHLIGHTER; a < OVERLAY_COUNT; a++) {
		overlay_textures[a].Apply([&](OverlayTexture_t &tex)
		{
			REK(tex);
		});
	}

	for ( int i = 0; i < INDICATE_NONE; i++ )
	{
		REK(m_pIndicatorTexture[i]);
	}

	LOG_MSG( "FreeRenderer: all done" LOG_CR );
}

void DX9Renderer::UpdateSquareBorderVB( IDirect3DVertexBuffer9 *pVB, int x, int y, int w, int h, DWORD color )
{
	const NEWVERTEX pVertSrc[] = 
	{
		{x, y, 0.0f, 1.0f, color, 0.0f, 0.0f }, // x, y, z, rhw, color, tu, tv
	    {x, y+h, 0.0f, 1.0f, color, 0.0f, 1.0f },
	    {x+w, y+h, 0.0f, 1.0f, color, 1.0f, 1.0f },
		{x+w, y, 0.0f, 1.0f, color, 1.0f, 0.0f },
		{x, y, 0.0f, 1.0f, color, 0.0f, 0.0f } // back to the start! since this is rendered as a line strip not triangles
	};
	int iSizeSrc = sizeof(pVertSrc);

	void* pVertices;
	
	HRESULT hRes = pVB->Lock( 0, iSizeSrc, &pVertices, 0 );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "UpdateVB: lock failed 0x%x" LOG_CR, hRes );
		return;
	}

	if ( pVertices == NULL )
	{
		LOG_MSG( "UpdateVB: lock succeeded but returned nothing?" LOG_CR );
		return;
	}

	memcpy( pVertices, pVertSrc, iSizeSrc );

	pVB->Unlock( );
}


void DX9Renderer::UpdateVB( IDirect3DVertexBuffer9 *pVB, int x, int y, int w, int h, DWORD color )
{
	const NEWVERTEX pVertSrc[] = 
	{
		{x-.5, y-.5, 0.0f, 1.0f, color, 0.0f, 0.0f }, // x, y, z, rhw, color, tu, tv
	    {x-.5, y+h-.5, 0.0f, 1.0f, color, 0.0f, 1.0f },
	    {x+w-.5, y-.5, 0.0f, 1.0f, color, 1.0f, 0.0f },
	    {x+w-.5, y+h-.5, 0.0f, 1.0f, color, 1.0f, 1.0f },
	};
	int iSizeSrc = sizeof(pVertSrc);

	void* pVertices;
	
	HRESULT hRes = pVB->Lock( 0, iSizeSrc, &pVertices, 0 );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "UpdateVB: lock failed 0x%x" LOG_CR, hRes );
		return;
	}

	if ( pVertices == NULL )
	{
		LOG_MSG( "UpdateVB: lock succeeded but returned nothing?" LOG_CR );
		return;
	}

	memcpy( pVertices, pVertSrc, iSizeSrc );

	pVB->Unlock( );
}

static struct {
	const D3DRENDERSTATETYPE state;
	const DWORD desired;
	DWORD backup;
} render_states[] = {
	{D3DRS_ZENABLE, D3DZB_FALSE},
	{D3DRS_ALPHABLENDENABLE, true},
	{D3DRS_FILLMODE, D3DFILL_SOLID},
	{D3DRS_CULLMODE, D3DCULL_NONE},
	{D3DRS_STENCILENABLE, false},
	{D3DRS_CLIPPING, true},
	{D3DRS_CLIPPLANEENABLE, false},
	{D3DRS_FOGENABLE, false},
	{D3DRS_LIGHTING, false},
	
	{D3DRS_SHADEMODE, D3DSHADE_GOURAUD},
	{D3DRS_ZWRITEENABLE, FALSE},
	{D3DRS_ZFUNC, D3DCMP_ALWAYS},
	
	// alpha/blending stuff (as given to us by half-life 2: lost cause - do we need to change them)
	{D3DRS_ALPHAFUNC, D3DCMP_GREATER},
	{D3DRS_SRCBLEND, D3DBLEND_SRCALPHA},
	{D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA},
	
	{D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_RED},
	{D3DRS_SRGBWRITEENABLE, true},
	
	{D3DRS_ALPHATESTENABLE, false},
	{D3DRS_SEPARATEALPHABLENDENABLE, false}, // this should make the next two obsolete according to the docs, but we'll keep them anyway
	{D3DRS_SRCBLENDALPHA, D3DBLEND_ONE},
	{D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO},
};

static struct {
	const D3DTEXTURESTAGESTATETYPE state;
	const DWORD desired;
	DWORD backup;
} texture_states[] = {
	{D3DTSS_COLOROP,   D3DTOP_MODULATE},
	{D3DTSS_COLORARG1, D3DTA_TEXTURE},
	{D3DTSS_COLORARG2, D3DTA_DIFFUSE},
	{D3DTSS_ALPHAOP,   D3DTOP_MODULATE},
	{D3DTSS_ALPHAARG1, D3DTA_TEXTURE},
	{D3DTSS_ALPHAARG2, D3DTA_DIFFUSE},
}, no_texture_states[] = {
	{D3DTSS_COLOROP, D3DTOP_DISABLE},
	{D3DTSS_ALPHAOP, D3DTOP_DISABLE},
};

static struct {
	const D3DSAMPLERSTATETYPE state;
	const DWORD desired;
	DWORD backup;
} sampler_states[] = {
	{D3DSAMP_MAGFILTER, D3DTEXF_LINEAR},
	{D3DSAMP_MINFILTER, D3DTEXF_LINEAR},
	{D3DSAMP_MIPFILTER, D3DTEXF_NONE},
	{D3DSAMP_SRGBTEXTURE, true},
};

void DX9Renderer::SetupRenderState( IDirect3DStateBlock9 **pStateBlock, DWORD vp_width, DWORD vp_height, bool textured )
{
	ProtectState([&]
	{
		D3DVIEWPORT9 vp;
		HRESULT hRes = m_pDevice->BeginStateBlock();

		vp.X = 0;
		vp.Y = 0;
		vp.Width = vp_width;
		vp.Height = vp_height;
		vp.MinZ = 0.0f;
		vp.MaxZ = 1.0f;

		m_pDevice->SetViewport(&vp);

		for (auto &rs : render_states)
			m_pDevice->SetRenderState(rs.state, rs.desired);

		if (textured)
		{
			for (auto &ts : texture_states)
				m_pDevice->SetTextureStageState(0, ts.state, ts.desired);

			for (auto &ss : sampler_states)
				m_pDevice->SetSamplerState(0, ss.state, ss.desired);
		}
		else
		{
			for (auto &nts : no_texture_states)
				m_pDevice->SetTextureStageState(0, nts.state, nts.desired);
		}

		m_pDevice->SetVertexShader(NULL);
		m_pDevice->SetFVF(D3DFVF_NEWVERTEX);
		m_pDevice->SetPixelShader(NULL);
		m_pDevice->SetStreamSource(0, m_pVBSquareBorder, 0, sizeof(NEWVERTEX));
		m_pDevice->EndStateBlock(pStateBlock);

		return true;
	});
}

void DX9Renderer::InitIndicatorTextures(IndicatorManager &manager, bool update)
{
	using namespace Gdiplus;

	for ( int i = 0; i < INDICATE_NONE; i++ )
	{
		auto ev = static_cast<IndicatorEvent>(i);
		if (update && !manager.ImageUpdated(ev))
			continue;

		Bitmap *bmp = manager.GetImage( i );
		BitmapData data;
	
		if ( !bmp )
		{
			LOG_MSG( "InitIndicatorTextures: invalid bitmap!" LOG_CR );
			return;
		}

		if ( bmp->LockBits(&Rect( 0, 0, bmp->GetWidth( ), bmp->GetHeight( ) ), ImageLockModeRead, PixelFormat32bppARGB, &data ) != Status::Ok )
		{
			LOG_MSG( "InitIndicatorTextures: bitmap[%d] data lock failed!" LOG_CR, i );
			return;
		}

		if ( m_pIndicatorTexture[i] )
			m_pIndicatorTexture[i].ReleaseRefObj();

		m_pDevice->CreateTexture( bmp->GetWidth( ), bmp->GetHeight( ), 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, IREF_GETPPTR(m_pIndicatorTexture[i], IDirect3DTexture9), NULL );
		if ( !m_pIndicatorTexture[i] )
		{
			LOG_WARN( "InitIndicatorTextures: couldn't create texture!" LOG_CR );
			break;
		}

		D3DLOCKED_RECT lr;
		HRESULT hr = m_pIndicatorTexture[i]->LockRect( 0, &lr, NULL, D3DLOCK_DISCARD );
		if ( FAILED(hr) )
		{
			LOG_MSG( "InitIndicatorTextures: texture data lock failed!" LOG_CR );
			return;
		}
	
		// these probably won't match, gpus are fussy about even dimensions and stuff. we have to copy line by line to compensate
		LOG_MSG( "InitIndicatorTextures: d3d surface pitch is %d, image stride is %d" LOG_CR, lr.Pitch, data.Stride );
		for ( UINT y = 0; y < data.Height; y++ )
			memcpy( (BYTE *)lr.pBits + (y * lr.Pitch), (BYTE *)data.Scan0 + (y * data.Stride), data.Stride );

		bmp->UnlockBits( &data );

		m_pIndicatorTexture[i]->UnlockRect( 0 );

		manager.ResetImageUpdated(ev);
	}
}

void DX9Renderer::DrawIndicator( TAKSI_INDICATE_TYPE eIndicate )
{
//#define LOGHR(hr, a) if ( FAILED(hr) ) { LOG_MSG( "DrawIndicator: %s failed with result 0x%x" LOG_CR, a, hr ); }
	// save the current render state
	HRESULT hRes = m_pCurrentRenderState->Capture( );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "DrawIndicator: state capture failed, aborting! 0x%08x" LOG_CR, hRes );
		return;
	}

	hRes = m_pSolidRenderState->Apply( );
	//LOGHR(hRes, "m_pSolidRenderState->Apply" );

	// update vertex buffer with new colours
	UpdateVB( m_pVBSquareIndicator, INDICATOR_X, INDICATOR_Y, INDICATOR_Width, INDICATOR_Height, sm_IndColors[eIndicate] );

	// render
	hRes = m_pDevice->BeginScene( );
	if ( SUCCEEDED(hRes) )
	{
		hRes = m_pDevice->SetStreamSource( 0, m_pVBSquareIndicator, 0, sizeof(NEWVERTEX) );
		//LOGHR(hRes, "m_pDevice->SetStreamSource(square)" );
		hRes = m_pDevice->DrawPrimitive( D3DPT_TRIANGLESTRIP, 0, 2 );
		//LOGHR(hRes, "m_pDevice->DrawPrimitive(triangles)" );

		hRes = m_pDevice->SetStreamSource( 0, m_pVBSquareBorder, 0, sizeof(NEWVERTEX) );
		//LOGHR(hRes, "m_pDevice->SetStreamSource(border)" );
		hRes = m_pDevice->DrawPrimitive( D3DPT_LINESTRIP, 0, 4 );
		//LOGHR(hRes, "m_pDevice->DrawPrimitive(lines)" );

		hRes = m_pDevice->EndScene( );
		//LOGHR(hRes, "m_pDevice->EndScene" );
	} 
	//else
		//LOG_MSG( "DrawIndicator: BeginScene failed with result 0x%x" LOG_CR, hRes );

	// restore render state
	m_pCurrentRenderState->Apply( );
	//LOGHR(hRes, "m_pCurrentRenderState->Apply" );
}

template <typename Fun>
bool DX9Renderer::ProtectState(Fun &&f)
{
	HRESULT hRes;

#undef CHEK
#define CHEK(x) \
    hRes = x; \
	if (FAILED(hRes)) \
	{ \
		LOG_WARN("RenderTex: " #x " failed! 0x%x", hRes); \
		return false; \
	}

	// setup renderstate
	CHEK(m_pCurrentRenderState->Capture());

	IRefPtr<IDirect3DPixelShader9> ps;
	CHEK(m_pDevice->GetPixelShader(ps.get_PPtr()));

	IRefPtr<IDirect3DVertexShader9> vs;
	CHEK(m_pDevice->GetVertexShader(vs.get_PPtr()));

	IRefPtr<IDirect3DSurface9> render_target;
	CHEK(m_pDevice->GetRenderTarget(0, render_target.get_PPtr()));

	// save whatever the current texture is. not doing this can break video cutscenes and stuff
	IRefPtr<IDirect3DBaseTexture9> pTexture;
	m_pDevice->GetTexture(0, pTexture.get_PPtr()); // note that it could be null

	for (auto &rs : render_states)
		CHEK(m_pDevice->GetRenderState(rs.state, &rs.backup));

	for (auto &ts : texture_states)
		CHEK(m_pDevice->GetTextureStageState(0, ts.state, &ts.backup));

	for (auto &nts : no_texture_states)
		CHEK(m_pDevice->GetTextureStageState(0, nts.state, &nts.backup));

	for (auto &ss : sampler_states)
		CHEK(m_pDevice->GetSamplerState(0, ss.state, &ss.backup));

	D3DVIEWPORT9 viewport;
	CHEK(m_pDevice->GetViewport(&viewport));

	DWORD fvf;
	CHEK(m_pDevice->GetFVF(&fvf));

	IRefPtr<IDirect3DVertexBuffer9> stream_data;
	UINT stream_offset;
	UINT stream_stride;
	CHEK(m_pDevice->GetStreamSource(0, stream_data.get_PPtr(), &stream_offset, &stream_stride));

	IRefPtr<IDirect3DSurface9> back_buffer;
	CHEK(m_pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back_buffer.get_PPtr()));

	bool render_target_set = false;
	if (back_buffer && back_buffer != render_target) {
		CHEK(m_pDevice->SetRenderTarget(0, back_buffer));
		render_target_set = true;
	}

	auto res = f();

	if (stream_data)
		m_pDevice->SetStreamSource(0, stream_data, stream_offset, stream_stride);

	m_pDevice->SetFVF(fvf);

	m_pDevice->SetViewport(&viewport);

	for (auto &ss : sampler_states)
		m_pDevice->SetSamplerState(0, ss.state, ss.backup);

	for (auto &nts : no_texture_states)
		m_pDevice->SetTextureStageState(0, nts.state, nts.backup);

	for (auto &ts : texture_states)
		m_pDevice->SetTextureStageState(0, ts.state, ts.backup);

	for (auto &rs : render_states)
		m_pDevice->SetRenderState(rs.state, rs.backup);

	// restore current texture if one was set
	if (pTexture)
		m_pDevice->SetTexture(0, pTexture);

	if (render_target_set)
		m_pDevice->SetRenderTarget(0, render_target);

	if (vs)
		m_pDevice->SetVertexShader(vs);

	if (ps)
		m_pDevice->SetPixelShader(ps);

	// restore the modified renderstate
	m_pCurrentRenderState->Apply();
	return res;
}

template <typename Fun>
bool DX9Renderer::RenderTex(Fun &&f)
{
	return ProtectState([&]
	{
		HRESULT hRes;

		m_pDevice->SetPixelShader(nullptr);
		m_pDevice->SetVertexShader(nullptr);

		if (m_pTexturedRenderState)
			m_pTexturedRenderState->Apply();

		// render
		hRes = m_pDevice->BeginScene();
		if (SUCCEEDED(hRes))
			f();

		return true;
	});
}

void DX9Renderer::DrawNewIndicator( IndicatorEvent eIndicatorEvent, DWORD color )
{
	if (eIndicatorEvent >= INDICATE_NONE)
		return;

	D3DSURFACE_DESC desc;
	m_pIndicatorTexture[eIndicatorEvent]->GetLevelDesc( 0, &desc );
	
	// new indicators show in top-right corner of the screen
	int x = g_Proc.m_Stats.m_SizeWnd.cx - desc.Width;
	int y = 0;

	UpdateVB( m_pVBNotification, x, y, desc.Width, desc.Height, color );
	
	RenderTex([&]
	{
		m_pDevice->SetStreamSource( 0, m_pVBNotification, 0, sizeof(NEWVERTEX) );
		m_pDevice->SetFVF( D3DFVF_NEWVERTEX );
		m_pDevice->SetTexture( 0, m_pIndicatorTexture[eIndicatorEvent] );
		m_pDevice->DrawPrimitive( D3DPT_TRIANGLESTRIP, 0, 2 );

		m_pDevice->EndScene();
	});
}

bool DX9Renderer::DrawOverlay(ActiveOverlay active_overlay)
{
	return overlay_textures[active_overlay].Draw([&](OverlayTexture_t &tex)
	{
		if (!RenderTex([&]
		{
			m_pDevice->SetStreamSource(0, m_pVBOverlay, 0, sizeof(NEWVERTEX));
			m_pDevice->SetFVF(D3DFVF_NEWVERTEX);
			m_pDevice->SetTexture(0, tex.get_RefObj());
			m_pDevice->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

			m_pDevice->EndScene();
		}))
			return false;

		return true;
	});
}

void DX9Renderer::UpdateOverlay()
{
	if (indicatorManager.updateTextures) {
		indicatorManager.updateTextures = false;
		InitIndicatorTextures(indicatorManager, true);
	}

	for (size_t i = OVERLAY_HIGHLIGHTER; i < OVERLAY_COUNT; i++)
		overlay_textures[i].Buffer([&](OverlayTexture_t &tex)
		{
			auto vec = ReadNewFramebuffer(static_cast<ActiveOverlay>(i));
			if (vec)
				/*hlog("Got vec %p: %d vs %dx%dx4 = %d", vec, vec->size(), g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy,
					g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4)*/;
			else
				return false;

			D3DLOCKED_RECT lr;
			HRESULT hr = tex->LockRect(0, &lr, NULL, D3DLOCK_DISCARD);
			if (FAILED(hr))
			{
				LOG_MSG("InitIndicatorTextures: texture data lock (%d) failed!" LOG_CR, i);
				return false;
			}

			// these probably won't match, gpus are fussy about even dimensions and stuff. we have to copy line by line to compensate
			//LOG_MSG("InitIndicatorTextures: d3d surface pitch is %d, image stride is %d" LOG_CR, lr.Pitch, data.Stride);
			for (UINT y = 0; y < g_Proc.m_Stats.m_SizeWnd.cy; y++)
				memcpy((BYTE *)lr.pBits + (y * lr.Pitch), (BYTE *)vec->data() + (y * g_Proc.m_Stats.m_SizeWnd.cx * 4), g_Proc.m_Stats.m_SizeWnd.cx * 4);


			tex->UnlockRect(0);
			return true;
		});
}

static bool get_back_buffer_size(IDirect3DDevice9 *dev, LONG &cx, LONG &cy)
{
	IDirect3DSurface9 *back_buffer = nullptr;

	auto hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer);
	if (FAILED(hr)) {
		return false;
	}

	D3DSURFACE_DESC desc;

	hr = back_buffer->GetDesc(&desc);
	back_buffer->Release();

	if (FAILED(hr)) {
		hlog("d3d9_init_format_backbuffer: Failed to get "
			"backbuffer descriptor (%#x)", hr);
		return false;
	}

	cx = desc.Width;
	cy = desc.Height;
	return true;
}

static bool get_size(IDirect3DDevice9 *dev, LONG &cx, LONG &cy, HWND &win)
{
	IDirect3DSwapChain9 *swap = nullptr;

	auto hr = dev->GetSwapChain(0, &swap);
	if (FAILED(hr)) {
		hlog("d3d9_get_swap_desc: Failed to get swap chain (%#x)", hr);
		return false;
	}

	D3DPRESENT_PARAMETERS pp;
	hr = swap->GetPresentParameters(&pp);
	swap->Release();

	if (FAILED(hr)) {
		hlog("d3d9_get_swap_desc: Failed to get "
			"presentation parameters (%#x)", hr);
		return false;
	}

	if (!get_back_buffer_size(dev, cx, cy)) {
		cx = pp.BackBufferWidth;
		cy = pp.BackBufferHeight;
	}

	win = pp.hDeviceWindow;
	return true;
}

using namespace std;

static DX9Renderer *renderer;
static bool initialized = false;
static HWND window = nullptr;
static mutex render_mutex;

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

void overlay_d3d9_free()
{
	LOCK(render_mutex);
	if (!initialized)
		return;

	delete renderer;
	renderer = nullptr;

	initialized = false;
}

static bool show_browser_tex(const ActiveOverlay &active_overlay = ::active_overlay)
{
	return renderer->DrawOverlay(active_overlay);
}

C_EXPORT void overlay_draw_d3d9(IDirect3DDevice9 *dev)
{
	LOCK(render_mutex);
	if (!initialized) {
		if (!(get_size(dev, g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy, window)))
			return;

		if (!renderer && !(renderer = new DX9Renderer{}))
			return;

		if (!renderer->InitRenderer(dev, indicatorManager)) {
			delete renderer;
			renderer = nullptr;
			return;
		}

		initialized = true;

		StartFramebufferServer();
	}

	HandleInputHook(window);

	renderer->UpdateOverlay();

	if (g_bBrowserShowing && show_browser_tex())
		return;

	ShowCurrentIndicator([&](IndicatorEvent indicator, BYTE alpha)
	{
		show_browser_tex(OVERLAY_NOTIFICATIONS);
		renderer->DrawNewIndicator(indicator, D3DCOLOR_ARGB(alpha, 255, 255, 255));
	});
}