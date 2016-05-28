// [GAPI_dx11_render.cpp 2013-09-05 abright]
// dx11 rendering stuff, separated from the capture code because there's so much of it

#include "stdafx.h"

#include "CImageGDIP.h"
#include "NewIndicator.h"

#include "../AnvilRendering.h"
//#include "TaksiDll.h"
//#include "GAPI_Base.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include "GAPI_dx11_render.h"

#include <memory>

extern pD3DCompile s_D3DCompile;

struct TEXMAPVERTEX 
{
	D3D11POS pos;
	D3D11COLOR color;
	D3D11TEX tex;
};

static const char* s_DX11TextureShader =
	"Texture2D txScreen : register( t0 );\r\n"
	"SamplerState samLinear : register( s0 );\r\n"

	"struct VS_INPUT\r\n"
	"{\r\n"
	"	float4 Pos : POSITION;\r\n"
	"	float4 Col : COLOR0;\r\n"
	"	float2 Tex : TEXCOORD0;\r\n"
	"};\r\n"

	"struct PS_INPUT\r\n"
	"{\r\n"
	"	float4 Pos : SV_POSITION;\r\n"
	"	float4 Col : COLOR0;\r\n"
	"	float2 Tex : TEXCOORD0;\r\n"
	"};\r\n"

	"PS_INPUT VS( VS_INPUT input )\r\n"
	"{\r\n"
	"    PS_INPUT output = (PS_INPUT)0;\r\n"
	"    output.Pos = input.Pos;\r\n"
	"    output.Col = input.Col;\r\n"
	"    output.Tex = input.Tex;  \r\n"
	"    return output;\r\n"
	"}\r\n"

	"float4 PS_Tex( PS_INPUT input ) : SV_Target\r\n"
	"{\r\n"
	"    return txScreen.Sample( samLinear, input.Tex ) * input.Col;\r\n"
	"}\r\n"

	"float4 PS_Solid( PS_INPUT input ) : SV_Target\r\n"
	"{\r\n"
	"    return input.Col;\r\n"
	"}\r\n";

DX11Renderer::DX11Renderer( ID3D11Device *pDevice )
{
	m_pDevice = pDevice;
}

DX11Renderer::~DX11Renderer( void )
{
	FreeRenderer( );
}

void DX11Renderer::InitIndicatorTextures( IndicatorManager &manager )
{
	using namespace Gdiplus;

	for ( int i = 0; i < INDICATE_NONE; i++ )
	{
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

		if ( m_pIndicatorTexture[i].IsValidRefObj( ) )
			m_pIndicatorTexture[i].ReleaseRefObj( );
		
		D3D11_TEXTURE2D_DESC desc;
		SecureZeroMemory( &desc, sizeof(D3D11_TEXTURE2D_DESC) );
		desc.Width = bmp->GetWidth( );
		desc.Height = bmp->GetHeight( );
		desc.ArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.Usage = D3D11_USAGE_IMMUTABLE;

		D3D11_SUBRESOURCE_DATA texData;
		texData.pSysMem = data.Scan0;
		texData.SysMemPitch = data.Stride;
		texData.SysMemSlicePitch = 0;

		HRESULT hRes;
		hRes = m_pDevice->CreateTexture2D( &desc, &texData, IREF_GETPPTR(m_pIndicatorTexture[i], ID3D11Texture2D) );
		if ( FAILED(hRes) )
		{
			LOG_WARN( "InitIndicatorTextures: couldn't create texture! 0x%08x" LOG_CR, hRes );
			bmp->UnlockBits( &data );
			break;
		}

		bmp->UnlockBits( &data );

		hRes = m_pDevice->CreateShaderResourceView( m_pIndicatorTexture[i], nullptr, IREF_GETPPTR(m_pResViewNotification[i], ID3D11ShaderResourceView) );
		if ( FAILED(hRes) )
		{
			LOG_WARN( "InitIndicatorTextures: couldn't create shader resource view! 0x%08x" LOG_CR, hRes );
			break;
		}
	}

	D3D11_TEXTURE2D_DESC desc;
	SecureZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
	desc.Width = g_Proc.m_Stats.m_SizeWnd.cx;
	desc.Height = g_Proc.m_Stats.m_SizeWnd.cy;
	desc.ArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	for (uint32_t a = OVERLAY_HIGHLIGHTER; a < OVERLAY_COUNT; a++) {
		overlay_textures[a].Apply([&](D3D11Texture &tex)
		{
			HRESULT hRes;
			hRes = m_pDevice->CreateTexture2D(&desc, nullptr, IREF_GETPPTR(tex.tex, ID3D11Texture2D));
			if (FAILED(hRes))
			{
				LOG_WARN("InitIndicatorTextures: couldn't create overlay texture! 0x%08x" LOG_CR, hRes);
				return;
			}

			hRes = m_pDevice->CreateShaderResourceView(tex.tex, nullptr, IREF_GETPPTR(tex.res, ID3D11ShaderResourceView));
			if (FAILED(hRes))
				LOG_WARN("InitIndicatorTextures: couldn't create overlay shader resource view! 0x%08x" LOG_CR, hRes);
		});
	}
}

void DX11Renderer::UpdateOverlayVB(ID3D11Texture2D *tex)
{
	IRefPtr<ID3D11DeviceContext> pContext;
	m_pDevice->GetImmediateContext(IREF_GETPPTR(pContext, ID3D11DeviceContext));

	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);

	UINT x, y, w, h;
	w = desc.Width;
	h = desc.Height;

	x = g_Proc.m_Stats.m_SizeWnd.cx - w;
	y = 0;

	float a = 1.;

	const TEXMAPVERTEX pVertSrc[] =
	{
		{ D3D11POS(x, y), D3D11COLOR(1.0, 1.0, 1.0, a), 0.0f, 0.0f }, // x, y, z, color, tu, tv
		{ D3D11POS(x, y + h), D3D11COLOR(1.0, 1.0, 1.0, a), 0.0f, 1.0f },
		{ D3D11POS(x + w, y), D3D11COLOR(1.0, 1.0, 1.0, a), 1.0f, 0.0f },
		{ D3D11POS(x + w, y + h), D3D11COLOR(1.0, 1.0, 1.0, a), 1.0f, 1.0f },
	};

	DWORD dwSize = sizeof(pVertSrc);

	D3D11_MAPPED_SUBRESOURCE map;
	HRESULT hRes = pContext->Map(m_pVBOverlay, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	if (SUCCEEDED(hRes))
	{
		CopyMemory(map.pData, pVertSrc, dwSize);
		pContext->Unmap(m_pVBOverlay, 0);
	}
}

void DX11Renderer::UpdateNotificationVB( IndicatorEvent eIndicatorEvent, BYTE alpha )
{
	IRefPtr<ID3D11DeviceContext> pContext;
	m_pDevice->GetImmediateContext( IREF_GETPPTR(pContext, ID3D11DeviceContext) );

	D3D11_TEXTURE2D_DESC desc;
	m_pIndicatorTexture[eIndicatorEvent]->GetDesc( &desc );

	UINT x, y, w, h;
	w = desc.Width;
	h = desc.Height;

	x = g_Proc.m_Stats.m_SizeWnd.cx - w;
	y = 0;

	float a = (float)alpha / 255;

	const TEXMAPVERTEX pVertSrc[] = 
	{
		{D3D11POS(x, y), D3D11COLOR(1.0, 1.0, 1.0, a), 0.0f, 0.0f }, // x, y, z, color, tu, tv
	    {D3D11POS(x, y+h), D3D11COLOR(1.0, 1.0, 1.0, a), 0.0f, 1.0f },
	    {D3D11POS(x+w, y), D3D11COLOR(1.0, 1.0, 1.0, a), 1.0f, 0.0f },
	    {D3D11POS(x+w, y+h), D3D11COLOR(1.0, 1.0, 1.0, a), 1.0f, 1.0f },
	};

	DWORD dwSize = sizeof(pVertSrc);

	D3D11_MAPPED_SUBRESOURCE map;
	HRESULT hRes = pContext->Map( m_pVBNotification, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );
	if ( SUCCEEDED(hRes) )
	{
		CopyMemory( map.pData, pVertSrc, dwSize );
		pContext->Unmap( m_pVBNotification, 0 );
	}
}

void DX11Renderer::UpdateSquareIndicatorVB( TAKSI_INDICATE_TYPE eIndicate )
{
	IRefPtr<ID3D11DeviceContext> pContext;
	m_pDevice->GetImmediateContext( IREF_GETPPTR(pContext, ID3D11DeviceContext) );

	UINT x, y, w, h;
	w = INDICATOR_Width;
	h = INDICATOR_Height;

	x = INDICATOR_X;
	y = INDICATOR_Y;

	DWORD colour = sm_IndColors[eIndicate];

	const TEXMAPVERTEX pVertInd[] = 
	{
		{ D3D11POS(INDICATOR_X+0.5f,				INDICATOR_Y+0.5f,					0.0f), D3D11COLOR(colour), 0.0f, 0.0f },	// top left
		{ D3D11POS(INDICATOR_X+INDICATOR_Width+0.5,	INDICATOR_Y+0.5f,					0.0f), D3D11COLOR(colour), 0.0f, 0.0f },	// top right
		{ D3D11POS(INDICATOR_X+0.5f,				INDICATOR_Y+INDICATOR_Height+0.5,	0.0f), D3D11COLOR(colour), 0.0f, 0.0f },	// bottom left
		{ D3D11POS(INDICATOR_X+INDICATOR_Width+0.5, INDICATOR_Y+INDICATOR_Height+0.5,	0.0f), D3D11COLOR(colour), 0.0f, 0.0f },	// bottom right
	};

	DWORD dwSize = sizeof(pVertInd);

	D3D11_MAPPED_SUBRESOURCE map;
	HRESULT hRes = pContext->Map( m_pVBSquareIndicator, 0, D3D11_MAP_WRITE_DISCARD, 0, &map );
	if ( SUCCEEDED(hRes) )
	{
		CopyMemory( map.pData, pVertInd, dwSize );
		pContext->Unmap( m_pVBSquareIndicator, 0 );
	}
}

HRESULT DX11Renderer::CreateIndicatorVB( void )
{
	const TEXMAPVERTEX pVertBorder[] =
	{
		{ D3D11POS(INDICATOR_X+0.5f,				INDICATOR_Y+0.5f,				 0.0f), D3D11COLOR(0xff000000), 0.0f, 0.0f },	// top left
		{ D3D11POS(INDICATOR_X+INDICATOR_Width+0.5, INDICATOR_Y+0.5f,				 0.0f), D3D11COLOR(0xff000000), 0.0f, 0.0f },	// top right
		{ D3D11POS(INDICATOR_X+INDICATOR_Width+0.5,	INDICATOR_Y+INDICATOR_Height+0.5,0.0f), D3D11COLOR(0xff000000), 0.0f, 0.0f },	// bottom right
		{ D3D11POS(INDICATOR_X+0.5f,				INDICATOR_Y+INDICATOR_Height+0.5,0.0f), D3D11COLOR(0xff000000), 0.0f, 0.0f },	// bottom left
		{ D3D11POS(INDICATOR_X+0.5f,				INDICATOR_Y+0.5f,				 0.0f), D3D11COLOR(0xff000000), 0.0f, 0.0f },	// top left
	};

	const TEXMAPVERTEX pVertInd[] = 
	{
		{ D3D11POS(INDICATOR_X+0.5f,				INDICATOR_Y+0.5f,					0.0f), D3D11COLOR(0xffffffff), 0.0f, 0.0f },	// top left
		{ D3D11POS(INDICATOR_X+INDICATOR_Width+0.5,	INDICATOR_Y+0.5f,					0.0f), D3D11COLOR(0xffffffff), 0.0f, 0.0f },	// top right
		{ D3D11POS(INDICATOR_X+0.5f,				INDICATOR_Y+INDICATOR_Height+0.5,	0.0f), D3D11COLOR(0xffffffff), 0.0f, 0.0f },	// bottom left
		{ D3D11POS(INDICATOR_X+INDICATOR_Width+0.5, INDICATOR_Y+INDICATOR_Height+0.5,	0.0f), D3D11COLOR(0xffffffff), 0.0f, 0.0f },	// bottom right
	};
	
	D3D11_BUFFER_DESC borderDesc;
	borderDesc.Usage = D3D11_USAGE_IMMUTABLE;
	borderDesc.ByteWidth = sizeof(pVertBorder);
	borderDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	borderDesc.CPUAccessFlags = 0;
	borderDesc.MiscFlags = 0;

	D3D11_SUBRESOURCE_DATA borderData;
	borderData.pSysMem = pVertBorder;

	D3D11_BUFFER_DESC indDesc;
	indDesc.Usage = D3D11_USAGE_DYNAMIC;
	indDesc.ByteWidth = sizeof(pVertInd);
	indDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	indDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	indDesc.MiscFlags = 0;

	D3D11_SUBRESOURCE_DATA indData;
	indData.pSysMem = pVertInd;
	
	HRESULT hRes;
	hRes = m_pDevice->CreateBuffer( &borderDesc, &borderData, IREF_GETPPTR(m_pVBSquareBorder, ID3D11Buffer) );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "DX11Renderer:CreateIndicatorVB: border buffer creation failed 0x%08x" LOG_CR, hRes );
		return hRes;
	}

	hRes = m_pDevice->CreateBuffer( &indDesc, &indData, IREF_GETPPTR(m_pVBSquareIndicator, ID3D11Buffer) );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "DX11Renderer:CreateIndicatorVB: indicator buffer creation failed 0x%08x" LOG_CR, hRes );
		return hRes;
	}

	return hRes;
}

bool DX11Renderer::InitRenderer( IndicatorManager &manager )
{
	#define CHEK(hr, a) if ( FAILED(hr) ) { LOG_MSG( "InitRenderer: %s failed with result 0x%08x" LOG_CR, a, hr ); return false; }
	HRESULT hRes;

	int w = g_Proc.m_Stats.m_SizeWnd.cx, h = g_Proc.m_Stats.m_SizeWnd.cy;

	// set up a rasterizer state we'll use
	D3D11_RASTERIZER_DESC rasterizerState;
    rasterizerState.FillMode = D3D11_FILL_SOLID;
    rasterizerState.CullMode = D3D11_CULL_NONE;
    rasterizerState.FrontCounterClockwise = true;
    rasterizerState.DepthBias = 0;
    rasterizerState.DepthBiasClamp = 0.0f;
    rasterizerState.SlopeScaledDepthBias = 0.0f;
    rasterizerState.DepthClipEnable = true;
    rasterizerState.ScissorEnable = false;
    rasterizerState.MultisampleEnable = false;
    rasterizerState.AntialiasedLineEnable = false;
	hRes = m_pDevice->CreateRasterizerState( &rasterizerState, &m_pRasterState );
	CHEK( hRes, "CreateRasterizerState" );

	// set up the depth/stencil state we want
	D3D11_DEPTH_STENCIL_DESC dsDesc;
	dsDesc.DepthEnable = false;
	dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
	dsDesc.StencilEnable = true;
	dsDesc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
	dsDesc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
	dsDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dsDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dsDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	hRes = m_pDevice->CreateDepthStencilState( &dsDesc, &m_pDepthState );
	CHEK( hRes, "CreateDepthStencilState" );

	// set up indicator textures
	InitIndicatorTextures( manager );

	// vertex buffers should go elsewhere?

	const TEXMAPVERTEX pVertSrc[] = 
	{
		{D3D11POS(0, 0), D3D11COLOR(0xffffffff), 0.0f, 0.0f }, // x, y, z, color, tu, tv
	    {D3D11POS(0, h), D3D11COLOR(0xffffffff), 0.0f, 1.0f },
	    {D3D11POS(w, 0), D3D11COLOR(0xffffffff), 1.0f, 0.0f },
	    {D3D11POS(w, h), D3D11COLOR(0xffffffff), 1.0f, 1.0f },
	};
	
	UINT iSizeSrc = sizeof(pVertSrc);
	
	D3D11_BUFFER_DESC BufferDescription;
	BufferDescription.Usage = D3D11_USAGE_DYNAMIC;
	BufferDescription.ByteWidth = iSizeSrc;
	BufferDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	BufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	BufferDescription.MiscFlags = 0;

	D3D11_SUBRESOURCE_DATA InitialData;
	InitialData.pSysMem = pVertSrc;
	
	hRes = m_pDevice->CreateBuffer(&BufferDescription, &InitialData, IREF_GETPPTR(m_pVBNotification, ID3D11Buffer));
	CHEK( hRes, "CreateBuffer" );

	hRes = m_pDevice->CreateBuffer(&BufferDescription, &InitialData, IREF_GETPPTR(m_pVBOverlay, ID3D11Buffer));
	CHEK(hRes, "CreateOverlayBuffer");

	hRes = CreateIndicatorVB( );
	CHEK( hRes, "CreateIndicatorVB" );

	IRefPtr<ID3DBlob> pVSBlob;
	UINT dwBufferSize = (UINT)strlen(s_DX11TextureShader) + 1;
	hRes = s_D3DCompile( s_DX11TextureShader, dwBufferSize, NULL, NULL, NULL, "VS", "vs_4_0", 0, 0, IREF_GETPPTR(pVSBlob, ID3DBlob), NULL );
	CHEK( hRes, "D3DCompile (vertex shader)" );

	IRefPtr<ID3DBlob> pPSTexBlob;
	hRes = s_D3DCompile( s_DX11TextureShader, dwBufferSize, NULL, NULL, NULL, "PS_Tex", "ps_4_0", 0, 0, IREF_GETPPTR(pPSTexBlob, ID3DBlob), NULL );
	CHEK( hRes, "D3DCompile (textured pixel shader)" );

	IRefPtr<ID3DBlob> pPSSolidBlob;
	hRes = s_D3DCompile( s_DX11TextureShader, dwBufferSize, NULL, NULL, NULL, "PS_Solid", "ps_4_0", 0, 0, IREF_GETPPTR(pPSSolidBlob, ID3DBlob), NULL );
	CHEK( hRes, "D3DCompile (solid pixel shader)" );

	hRes = m_pDevice->CreateVertexShader( pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), NULL, IREF_GETPPTR(m_pVertexShader,ID3D11VertexShader) );
	CHEK( hRes, "CreateVertexShader" );
	
	hRes = m_pDevice->CreatePixelShader( pPSTexBlob->GetBufferPointer(), pPSTexBlob->GetBufferSize(), NULL, IREF_GETPPTR(m_pPixelShader,ID3D11PixelShader) );
	CHEK( hRes, "CreatePixelShader" );

	hRes = m_pDevice->CreatePixelShader( pPSSolidBlob->GetBufferPointer(), pPSSolidBlob->GetBufferSize(), NULL, IREF_GETPPTR(m_pPixelShaderSolid,ID3D11PixelShader) );
	CHEK( hRes, "CreatePixelShader (solid)" );

	D3D11_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	UINT numElements = ARRAYSIZE( layout );
	hRes = m_pDevice->CreateInputLayout(layout, numElements, pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), IREF_GETPPTR(m_pVertexLayout, ID3D11InputLayout));
	CHEK( hRes, "CreateInputLayout" );
	
	D3D11_SAMPLER_DESC sampDesc;
    SecureZeroMemory( &sampDesc, sizeof(sampDesc) );
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hRes = m_pDevice->CreateSamplerState( &sampDesc, IREF_GETPPTR(m_pSamplerState, ID3D11SamplerState) );
	CHEK( hRes, "CreateSamplerState" );

	D3D11_BLEND_DESC blendDesc;
	SecureZeroMemory( &blendDesc, sizeof(D3D11_BLEND_DESC) );
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hRes = m_pDevice->CreateBlendState( &blendDesc, IREF_GETPPTR(m_pBlendStateTextured, ID3D11BlendState) );
	CHEK( hRes, "CreateBlendState" );

	// make a render target view of backbuffer
	/*
	IRefPtr<ID3D11Texture2D> pBackBuffer;
	hRes = pSwapChain->GetBuffer( 0, __uuidof(ID3D11Texture2D), IREF_GETPPTRV(pBackBuffer, ID3D11Texture2D) );
	CHEK( hRes, "SwapChain:GetBuffer");

	hRes = m_pDevice->CreateRenderTargetView( pBackBuffer, nullptr, IREF_GETPPTR(m_pRenderTargetView, ID3D11RenderTargetView) );
	CHEK( hRes, "CreateRenderTargetView" );
	*/

	return S_OK;
}

void DX11Renderer::FreeRenderer( void )
{
	m_pVBNotification.ReleaseRefObj( );
	m_pVBSquareIndicator.ReleaseRefObj( );
	m_pVBSquareBorder.ReleaseRefObj( );
	m_pVertexLayout.ReleaseRefObj( );
	m_pVertexShader.ReleaseRefObj( );
	m_pPixelShader.ReleaseRefObj( );
	m_pPixelShaderSolid.ReleaseRefObj( );
	
	m_pSamplerState.ReleaseRefObj( );
	//m_pRenderTargetView.ReleaseRefObj( );
	m_pBlendStateTextured.ReleaseRefObj( );

	for( UINT i = 0; i < INDICATE_NONE; i++ )
	{
		m_pIndicatorTexture[i].ReleaseRefObj( );
		m_pResViewNotification[i].ReleaseRefObj( );
	}
}

void DX11Renderer::DrawNewIndicator( IDXGISwapChain *pSwapChain, IndicatorEvent eIndicatorEvent, BYTE alpha )
{
	IRefPtr<ID3D11DeviceContext> pContext;
	m_pDevice->GetImmediateContext( IREF_GETPPTR(pContext, ID3D11DeviceContext) );

	HRESULT hRes;
	IRefPtr<ID3D11Texture2D> pBackBuffer;
	hRes = pSwapChain->GetBuffer( 0, __uuidof(ID3D11Texture2D), IREF_GETPPTRV(pBackBuffer, ID3D11Texture2D) );
	if ( FAILED(hRes) ) 
	{ 
		LOG_MSG( "DrawNewIndicator: pSwapChain->GetBuffer failed with result 0x%08x" LOG_CR, hRes ); 
		return; 
	}
	
	IRefPtr<ID3D11RenderTargetView> pRenderTargetView;
	hRes = m_pDevice->CreateRenderTargetView( pBackBuffer, nullptr, IREF_GETPPTR(pRenderTargetView, ID3D11RenderTargetView) );
	if ( FAILED(hRes) ) 
	{ 
		LOG_MSG( "DrawNewIndicator: pSwapChain->GetBuffer failed with result 0x%08x" LOG_CR, hRes ); 
		return; 
	}

	D3D11_VIEWPORT vp;
	vp.TopLeftX      = 0;
	vp.TopLeftY      = 0;
	vp.Width  = (UINT)( g_Proc.m_Stats.m_SizeWnd.cx );
	vp.Height = (UINT)( g_Proc.m_Stats.m_SizeWnd.cy );
	vp.MinDepth   = 0.0f;
	vp.MaxDepth   = 1.0f;

// save current state
	IRefPtr<ID3D11RasterizerState> pSavedRSState;
	pContext->RSGetState( IREF_GETPPTR(pSavedRSState, ID3D11RasterizerState) );

	IRefPtr<ID3D11DepthStencilState> pSavedDepthState;
	UINT pStencilRef = 0;
	pContext->OMGetDepthStencilState(IREF_GETPPTR(pSavedDepthState, ID3D11DepthStencilState), &pStencilRef);

	// will games with multiple viewports break here?
	D3D11_VIEWPORT originalVP;
	UINT numViewPorts = 1;
	pContext->RSGetViewports(&numViewPorts, &originalVP);

	// save render/depth target so we can restore after
	IRefPtr<ID3D11RenderTargetView> pOldRenderTargetView;
	IRefPtr<ID3D11DepthStencilView> pDepthStencilView;
	pContext->OMGetRenderTargets(1, IREF_GETPPTR(pOldRenderTargetView, ID3D11RenderTargetView), IREF_GETPPTR(pDepthStencilView, ID3D11DepthStencilView));

	IRefPtr<ID3D11BlendState> pBlendState;
	float fBlendFactor[4];
	UINT uSampleMask;
	pContext->OMGetBlendState( IREF_GETPPTR(pBlendState, ID3D11BlendState), fBlendFactor, &uSampleMask );

	D3D11_PRIMITIVE_TOPOLOGY old_topology;
	pContext->IAGetPrimitiveTopology( &old_topology );

	IRefPtr<ID3D11InputLayout> pOldInputLayout;
	pContext->IAGetInputLayout( IREF_GETPPTR(pOldInputLayout, ID3D11InputLayout) );	

// setup
	pContext->OMSetRenderTargets(1, pRenderTargetView.get_Array(), nullptr );
	pContext->RSSetViewports(1, &vp);						// Set the new viewport for the indicator
	pContext->RSSetState(m_pRasterState);					// Set the new state
	pContext->OMSetDepthStencilState(m_pDepthState, 1);		// Set depth-stencil state to temporarily disable z-buffer

	float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	pContext->OMSetBlendState( m_pBlendStateTextured, blend_factor, 0xffffffff );

	pContext->IASetInputLayout( m_pVertexLayout );
	
	// todo: save current vertex/pixel shaders to restore after drawing
	// see crysis 3 intro sequence for why (green screen where it tries to do shit using our shader?)

	pContext->VSSetShader( m_pVertexShader, NULL, 0 );
	pContext->PSSetShader( m_pPixelShader, NULL, 0 );

	pContext->PSSetShaderResources( 0, 1, m_pResViewNotification[eIndicatorEvent].get_Array() );
	pContext->PSSetSamplers( 0, 1, m_pSamplerState.get_Array() );

	UpdateNotificationVB( eIndicatorEvent, alpha );

// draw
	UINT stride = sizeof( TEXMAPVERTEX );
	UINT offset = 0;
	pContext->IASetVertexBuffers( 0, 1, m_pVBNotification.get_Array(), &stride, &offset );
	pContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
	pContext->Draw( 4, 0 );

// restore state
	pContext->IASetPrimitiveTopology( old_topology );
	pContext->IASetInputLayout( pOldInputLayout.get_RefObj() );	

	auto targetView = pOldRenderTargetView.get_RefObj();
	pContext->OMSetRenderTargets(1, &targetView, pDepthStencilView);

	pContext->RSSetViewports(1, &originalVP);				// restore the old viewport
	pContext->RSSetState(pSavedRSState);					// restore the old state
	pContext->OMSetDepthStencilState(pSavedDepthState, 1);	// restore the old depth state (re-enable zbuffer)

	pContext->OMSetBlendState( pBlendState, fBlendFactor, uSampleMask );

	pRenderTargetView.ReleaseRefObj( );
	pOldRenderTargetView.ReleaseRefObj( );
	pOldInputLayout.ReleaseRefObj( );
}

void DX11Renderer::DrawIndicator( IDXGISwapChain *pSwapChain, TAKSI_INDICATE_TYPE eIndicate )
{
	IRefPtr<ID3D11DeviceContext> pContext;
	m_pDevice->GetImmediateContext( IREF_GETPPTR(pContext, ID3D11DeviceContext) );

	HRESULT hRes;
	IRefPtr<ID3D11Texture2D> pBackBuffer;
	hRes = pSwapChain->GetBuffer( 0, __uuidof(ID3D11Texture2D), IREF_GETPPTRV(pBackBuffer, ID3D11Texture2D) );
	if ( FAILED(hRes) ) 
	{ 
		LOG_MSG( "DrawNewIndicator: pSwapChain->GetBuffer failed with result 0x%08x" LOG_CR, hRes ); 
		return; 
	}
	
	IRefPtr<ID3D11RenderTargetView> pRenderTargetView;
	hRes = m_pDevice->CreateRenderTargetView( pBackBuffer, nullptr, IREF_GETPPTR(pRenderTargetView, ID3D11RenderTargetView) );
	if ( FAILED(hRes) ) 
	{ 
		LOG_MSG( "DrawNewIndicator: pSwapChain->GetBuffer failed with result 0x%08x" LOG_CR, hRes ); 
		return; 
	}

	D3D11_VIEWPORT vp;
	vp.TopLeftX      = 0;
	vp.TopLeftY      = 0;
	vp.Width  = (UINT)( INDICATOR_X*2 + INDICATOR_Width );
	vp.Height = (UINT)( INDICATOR_Y*2 + INDICATOR_Height );
	vp.MinDepth   = 0.0f;
	vp.MaxDepth   = 1.0f;

// save current state
	IRefPtr<ID3D11RasterizerState> pSavedRSState;
	pContext->RSGetState( IREF_GETPPTR(pSavedRSState, ID3D11RasterizerState) );

	IRefPtr<ID3D11DepthStencilState> pSavedDepthState;
	UINT pStencilRef = 0;
	pContext->OMGetDepthStencilState(IREF_GETPPTR(pSavedDepthState, ID3D11DepthStencilState), &pStencilRef);

	// will games with multiple viewports break here?
	D3D11_VIEWPORT originalVP;
	UINT numViewPorts = 1;
	pContext->RSGetViewports(&numViewPorts, &originalVP);

	// save render/depth target so we can restore after
	IRefPtr<ID3D11RenderTargetView> pOldRenderTargetView;
	IRefPtr<ID3D11DepthStencilView> pDepthStencilView;
	pContext->OMGetRenderTargets(1, IREF_GETPPTR(pOldRenderTargetView, ID3D11RenderTargetView), IREF_GETPPTR(pDepthStencilView, ID3D11DepthStencilView));

	IRefPtr<ID3D11BlendState> pBlendState;
	float fBlendFactor[4];
	UINT uSampleMask;
	pContext->OMGetBlendState( IREF_GETPPTR(pBlendState, ID3D11BlendState), fBlendFactor, &uSampleMask );

	D3D11_PRIMITIVE_TOPOLOGY old_topology;
	pContext->IAGetPrimitiveTopology( &old_topology );

	IRefPtr<ID3D11InputLayout> pOldInputLayout;
	pContext->IAGetInputLayout( IREF_GETPPTR(pOldInputLayout, ID3D11InputLayout) );	

// setup
	pContext->OMSetRenderTargets(1, IREF_GETPPTR(pRenderTargetView, ID3D11RenderTargetView), nullptr );
	pContext->RSSetViewports(1, &vp);						// Set the new viewport for the indicator
	pContext->RSSetState(m_pRasterState);					// Set the new state
	pContext->OMSetDepthStencilState(m_pDepthState, 1);		// Set depth-stencil state to temporarily disable z-buffer

	float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	pContext->OMSetBlendState( m_pBlendStateTextured, blend_factor, 0xffffffff );

	pContext->IASetInputLayout( m_pVertexLayout );
	
	pContext->VSSetShader( m_pVertexShader, NULL, 0 );
	pContext->PSSetShader( m_pPixelShaderSolid, NULL, 0 );

	UpdateSquareIndicatorVB( eIndicate );

// draw
	UINT stride = sizeof( TEXMAPVERTEX );
	UINT offset = 0;
	pContext->IASetVertexBuffers( 0, 1, IREF_GETPPTR(m_pVBSquareIndicator, ID3D11Buffer), &stride, &offset );
	pContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
	pContext->Draw( 4, 0 );

	pContext->IASetVertexBuffers( 0, 1, IREF_GETPPTR(m_pVBSquareBorder, ID3D11Buffer), &stride, &offset );
	pContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP );
	pContext->Draw( 5, 0 );


// restore state
	pContext->IASetPrimitiveTopology( old_topology );
	pContext->IASetInputLayout( pOldInputLayout.get_RefObj() );	

	pContext->OMSetRenderTargets(1, IREF_GETPPTR(pOldRenderTargetView, ID3D11RenderTargetView), pDepthStencilView);

	pContext->RSSetViewports(1, &originalVP);				// restore the old viewport
	pContext->RSSetState(pSavedRSState);					// restore the old state
	pContext->OMSetDepthStencilState(pSavedDepthState, 1);	// restore the old depth state (re-enable zbuffer)

	pContext->OMSetBlendState( pBlendState, fBlendFactor, uSampleMask );

	pRenderTargetView.ReleaseRefObj( );
	pOldRenderTargetView.ReleaseRefObj( );
	pOldInputLayout.ReleaseRefObj( );
}

bool DX11Renderer::DrawOverlay(IDXGISwapChain *pSwapChain)
{
	return overlay_textures[active_overlay].Draw([&](D3D11Texture &tex)
	{
		IRefPtr<ID3D11DeviceContext> pContext;
		m_pDevice->GetImmediateContext(IREF_GETPPTR(pContext, ID3D11DeviceContext));

		HRESULT hRes;
		IRefPtr<ID3D11Texture2D> pBackBuffer;
		hRes = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), IREF_GETPPTRV(pBackBuffer, ID3D11Texture2D));
		if (FAILED(hRes))
		{
			LOG_MSG("DrawOverlay: pSwapChain->GetBuffer failed with result 0x%08x" LOG_CR, hRes);
			return false;
		}

		IRefPtr<ID3D11RenderTargetView> pRenderTargetView;
		hRes = m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, IREF_GETPPTR(pRenderTargetView, ID3D11RenderTargetView));
		if (FAILED(hRes))
		{
			LOG_MSG("DrawOverlay: pSwapChain->GetBuffer failed with result 0x%08x" LOG_CR, hRes);
			return false;
		}

		D3D11_VIEWPORT vp;
		vp.TopLeftX = 0;
		vp.TopLeftY = 0;
		vp.Width = (UINT)(g_Proc.m_Stats.m_SizeWnd.cx);
		vp.Height = (UINT)(g_Proc.m_Stats.m_SizeWnd.cy);
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;

		// save current state
		IRefPtr<ID3D11RasterizerState> pSavedRSState;
		pContext->RSGetState(IREF_GETPPTR(pSavedRSState, ID3D11RasterizerState));

		IRefPtr<ID3D11DepthStencilState> pSavedDepthState;
		UINT pStencilRef = 0;
		pContext->OMGetDepthStencilState(IREF_GETPPTR(pSavedDepthState, ID3D11DepthStencilState), &pStencilRef);

		// will games with multiple viewports break here?
		D3D11_VIEWPORT originalVP;
		UINT numViewPorts = 1;
		pContext->RSGetViewports(&numViewPorts, &originalVP);

		// save render/depth target so we can restore after
		IRefPtr<ID3D11RenderTargetView> pOldRenderTargetView;
		IRefPtr<ID3D11DepthStencilView> pDepthStencilView;
		pContext->OMGetRenderTargets(1, IREF_GETPPTR(pOldRenderTargetView, ID3D11RenderTargetView), IREF_GETPPTR(pDepthStencilView, ID3D11DepthStencilView));

		IRefPtr<ID3D11BlendState> pBlendState;
		float fBlendFactor[4];
		UINT uSampleMask;
		pContext->OMGetBlendState(IREF_GETPPTR(pBlendState, ID3D11BlendState), fBlendFactor, &uSampleMask);

		D3D11_PRIMITIVE_TOPOLOGY old_topology;
		pContext->IAGetPrimitiveTopology(&old_topology);

		IRefPtr<ID3D11InputLayout> pOldInputLayout;
		pContext->IAGetInputLayout(IREF_GETPPTR(pOldInputLayout, ID3D11InputLayout));

		// setup
		pContext->OMSetRenderTargets(1, pRenderTargetView.get_Array(), nullptr);
		pContext->RSSetViewports(1, &vp);						// Set the new viewport for the indicator
		pContext->RSSetState(m_pRasterState);					// Set the new state
		pContext->OMSetDepthStencilState(m_pDepthState, 1);		// Set depth-stencil state to temporarily disable z-buffer

		float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		pContext->OMSetBlendState(m_pBlendStateTextured, blend_factor, 0xffffffff);

		pContext->IASetInputLayout(m_pVertexLayout);

		// todo: save current vertex/pixel shaders to restore after drawing
		// see crysis 3 intro sequence for why (green screen where it tries to do shit using our shader?)

		pContext->VSSetShader(m_pVertexShader, NULL, 0);
		pContext->PSSetShader(m_pPixelShader, NULL, 0);

		pContext->PSSetShaderResources(0, 1, tex.res.get_Array());
		pContext->PSSetSamplers(0, 1, m_pSamplerState.get_Array());

		UpdateOverlayVB(tex.tex);

		// draw
		UINT stride = sizeof(TEXMAPVERTEX);
		UINT offset = 0;
		pContext->IASetVertexBuffers(0, 1, m_pVBOverlay.get_Array(), &stride, &offset);
		pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		pContext->Draw(4, 0);

		// restore state
		pContext->IASetPrimitiveTopology(old_topology);
		pContext->IASetInputLayout(pOldInputLayout.get_RefObj());

		auto targetView = pOldRenderTargetView.get_RefObj();
		pContext->OMSetRenderTargets(1, &targetView, pDepthStencilView);

		pContext->RSSetViewports(1, &originalVP);				// restore the old viewport
		pContext->RSSetState(pSavedRSState);					// restore the old state
		pContext->OMSetDepthStencilState(pSavedDepthState, 1);	// restore the old depth state (re-enable zbuffer)

		pContext->OMSetBlendState(pBlendState, fBlendFactor, uSampleMask);

		pRenderTargetView.ReleaseRefObj();
		pOldRenderTargetView.ReleaseRefObj();
		pOldInputLayout.ReleaseRefObj();
		return true;
	});
}

void DX11Renderer::UpdateOverlay()
{
	for (size_t i = OVERLAY_HIGHLIGHTER; i < OVERLAY_COUNT; i++)
		overlay_textures[i].Buffer([&](D3D11Texture &tex)
		{
			auto vec = ReadNewFramebuffer(static_cast<ActiveOverlay>(i));
			if (vec)
				/*hlog("Got vec %p: %d vs %dx%dx4 = %d", vec, vec->size(), g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy,
					g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4)*/;
			else
				return false;

			IRefPtr<ID3D11DeviceContext> pContext;
			m_pDevice->GetImmediateContext(IREF_GETPPTR(pContext, ID3D11DeviceContext));

			D3D11_MAPPED_SUBRESOURCE mr;
			ZeroMemory(&mr, sizeof(D3D11_MAPPED_SUBRESOURCE));

			auto hr = pContext->Map(tex.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
			if (FAILED(hr))
			{
				LOG_MSG("UpdateOverlay: texture data lock (%d) failed!" LOG_CR, i);
				return false;
			}

			// these probably won't match, gpus are fussy about even dimensions and stuff. we have to copy line by line to compensate
			//LOG_MSG("InitIndicatorTextures: d3d surface pitch is %d, image stride is %d" LOG_CR, lr.Pitch, data.Stride);
			for (UINT y = 0; y < g_Proc.m_Stats.m_SizeWnd.cy; y++)
				memcpy((BYTE *)mr.pData + (y * mr.RowPitch), (BYTE *)vec->data() + (y * g_Proc.m_Stats.m_SizeWnd.cx * 4), g_Proc.m_Stats.m_SizeWnd.cx * 4);

			pContext->Unmap(tex.tex, 0);

			return true;
		});
}

using namespace std;
static unique_ptr<DX11Renderer> renderer;
static HWND window = nullptr;

static DX11Renderer *get_renderer(IDXGISwapChain *swap)
{
	if (!renderer)
	{
		IRefPtr<ID3D11Device> dev;
		auto hr = swap->GetDevice(__uuidof(ID3D11Device), (void**)IREF_GETPPTR(dev, ID3D11Device));
		if (FAILED(hr) || !dev)
			return nullptr;

		DXGI_SWAP_CHAIN_DESC desc;
		hr = swap->GetDesc(&desc);
		if (FAILED(hr))
			return nullptr;

		g_Proc.m_Stats.m_SizeWnd.cx = desc.BufferDesc.Width;
		g_Proc.m_Stats.m_SizeWnd.cy = desc.BufferDesc.Height;
		window = desc.OutputWindow;

		renderer.reset(new DX11Renderer{dev}); //release dev?
		renderer->InitRenderer(indicatorManager);

		StartFramebufferServer();
	}

	return renderer.get();
}

void overlay_d3d11_free()
{
	renderer.reset();
}

static bool show_browser_tex(IDXGISwapChain *swap)
{
	return renderer->DrawOverlay(swap);
}

C_EXPORT void overlay_draw_d3d11(IDXGISwapChain *swap)
{
	auto renderer = get_renderer(swap);
	if (!renderer)
		return;

	HandleInputHook(window);

	renderer->UpdateOverlay();

	if (!g_bBrowserShowing || !show_browser_tex(swap))
	ShowCurrentIndicator([&](IndicatorEvent indicator, BYTE alpha)
	{
		renderer->DrawNewIndicator(swap, indicator, alpha);
	});
}