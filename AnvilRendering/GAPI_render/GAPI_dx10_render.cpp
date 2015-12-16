// [GAPI_dx10_render.cpp 2014-09-03 abright]
// dx10 rendering, based almost entirely on the dx11 render code

#include "stdafx.h"
#include "../AnvilRendering.h"
//#include "TaksiDll.h"
//#include "GAPI_Base.h"

#include <d3d10.h>

#include "CImageGDIP.h"
#include "NewIndicator.h"

#include "GAPI_dx10_render.h"

#include <memory>

typedef HRESULT (WINAPI *PFN_D3D10_COMPILESHADER)(LPCSTR pSrcData, SIZE_T SrcDataSize, LPCSTR pFileName, CONST D3D10_SHADER_MACRO* pDefines, LPD3D10INCLUDE pInclude, 
    LPCSTR pFunctionName, LPCSTR pProfile, UINT Flags, ID3D10Blob** ppShader, ID3D10Blob** ppErrorMsgs);

static PFN_D3D10_COMPILESHADER s_D3D10CompileShader;

bool D3D10_LoadFunctions( void )
{
	HMODULE hDX10 = GetModuleHandle( L"d3d10.dll" );
	if (!hDX10)
	{
		LOG_WARN( "D3D10_LoadFunctions: unable to find D3D10 library. Not loaded?" LOG_CR );
		return false;
	}

	s_D3D10CompileShader = reinterpret_cast<PFN_D3D10_COMPILESHADER>( GetProcAddress( hDX10, "D3D10CompileShader" ) );
	if ( !s_D3D10CompileShader )
	{
		LOG_WARN( "D3D10_LoadFunctions: unable to load required D3D10 functions" LOG_CR );
		return false;
	}

	return true;
}

struct TEXMAPVERTEX 
{
	D3D10POS pos;
	D3D10COLOR color;
	D3D10TEX tex;
};

static const char* s_DX10TextureShader =
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

DX10Renderer::DX10Renderer( ID3D10Device *pDevice )
{
	m_pDevice = pDevice;
}

void DX10Renderer::InitIndicatorTextures( IndicatorManager &manager )
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

		void *pData = HeapAlloc( GetProcessHeap( ), HEAP_ZERO_MEMORY, data.Stride * data.Height );
		if (!pData )
		{
			LOG_WARN( "InitIndicatorTextures: couldn't create temp buffer for texture[%d]!" LOG_CR );
			return;
		}

		// BGR to RGB bs. D3D10 doesn't support DXGI_FORMAT_B8G8R8A8_UNORM (but 10.1 does) so we've got to convert shit manually here
		// copying to a temp buffer then doing in-place conversion isn't neat but we're working with small images and shouldn't be doing this often
		CopyMemory( pData, data.Scan0, data.Stride * data.Height );
		for ( UINT p = 0; p < data.Width * data.Height; p++ )
		{
			BYTE *pixel = (BYTE *)pData + (4 * p);
			BYTE r = pixel[2];
			pixel[2] = pixel[0];
			pixel[0] = r;
		}

		if ( m_pIndicatorTexture[i].IsValidRefObj( ) )
			m_pIndicatorTexture[i].ReleaseRefObj( );
		
		LOG_MSG( "InitIndicatorTextures: bitmap[%d] size is %ux%u, stride %u, data %p" LOG_CR, i, bmp->GetWidth( ), bmp->GetHeight( ), data.Stride, data.Scan0 );
	
		D3D10_TEXTURE2D_DESC desc;
		SecureZeroMemory( &desc, sizeof(D3D10_TEXTURE2D_DESC) );
		desc.Width = data.Width;
		desc.Height = data.Height;
		desc.ArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.Usage = D3D10_USAGE_IMMUTABLE;
		
		D3D10_SUBRESOURCE_DATA texData;
		texData.pSysMem = pData;
		texData.SysMemPitch = data.Stride;
		texData.SysMemSlicePitch = 0;

		HRESULT hRes;
		hRes = m_pDevice->CreateTexture2D( &desc, &texData, IREF_GETPPTR(m_pIndicatorTexture[i], ID3D11Texture2D) );
		if ( FAILED(hRes) )
		{
			LOG_WARN( "InitIndicatorTextures: couldn't create texture[%d]! 0x%08x" LOG_CR, i, hRes );
			HeapFree( GetProcessHeap( ), 0, pData );
			bmp->UnlockBits( &data );
			break;
		}

		HeapFree( GetProcessHeap( ), 0, pData );
		bmp->UnlockBits( &data );

		hRes = m_pDevice->CreateShaderResourceView( m_pIndicatorTexture[i], nullptr, IREF_GETPPTR(m_pResViewNotification[i], ID3D11ShaderResourceView) );
		if ( FAILED(hRes) )
		{
			LOG_WARN( "InitIndicatorTextures: couldn't create shader resource view[%d]! 0x%08x" LOG_CR, i, hRes );
			break;
		}
	}
}

void DX10Renderer::UpdateNotificationVB( IndicatorEvent eIndicatorEvent, BYTE alpha )
{
	D3D10_TEXTURE2D_DESC desc;
	m_pIndicatorTexture[eIndicatorEvent]->GetDesc( &desc );

	UINT x, y, w, h;
	w = desc.Width;
	h = desc.Height;

	x = g_Proc.m_Stats.m_SizeWnd.cx - w;
	y = 0;

	float a = (float)alpha / 255;

	const TEXMAPVERTEX pVertSrc[] = 
	{
		{D3D10POS(x, y), D3D10COLOR(1.0, 1.0, 1.0, a), 0.0f, 0.0f }, // x, y, z, color, tu, tv
	    {D3D10POS(x, y+h), D3D10COLOR(1.0, 1.0, 1.0, a), 0.0f, 1.0f },
	    {D3D10POS(x+w, y), D3D10COLOR(1.0, 1.0, 1.0, a), 1.0f, 0.0f },
	    {D3D10POS(x+w, y+h), D3D10COLOR(1.0, 1.0, 1.0, a), 1.0f, 1.0f },
	};

	DWORD dwSize = sizeof(pVertSrc);

	void *pData = nullptr;
	HRESULT hRes = m_pVBNotification->Map( D3D10_MAP_WRITE_DISCARD, 0, &pData );
	if ( SUCCEEDED(hRes) )
	{
		CopyMemory( pData, pVertSrc, dwSize );
		m_pVBNotification->Unmap( );
	}
}

void DX10Renderer::UpdateSquareIndicatorVB( TAKSI_INDICATE_TYPE eIndicate )
{
	UINT x, y, w, h;
	w = INDICATOR_Width;
	h = INDICATOR_Height;

	x = INDICATOR_X;
	y = INDICATOR_Y;

	DWORD colour = sm_IndColors[eIndicate];

	const TEXMAPVERTEX pVertInd[] = 
	{
		{ D3D10POS(INDICATOR_X+0.5f,				INDICATOR_Y+0.5f,					0.0f), D3D10COLOR(colour), 0.0f, 0.0f },	// top left
		{ D3D10POS(INDICATOR_X+INDICATOR_Width+0.5,	INDICATOR_Y+0.5f,					0.0f), D3D10COLOR(colour), 0.0f, 0.0f },	// top right
		{ D3D10POS(INDICATOR_X+0.5f,				INDICATOR_Y+INDICATOR_Height+0.5,	0.0f), D3D10COLOR(colour), 0.0f, 0.0f },	// bottom left
		{ D3D10POS(INDICATOR_X+INDICATOR_Width+0.5, INDICATOR_Y+INDICATOR_Height+0.5,	0.0f), D3D10COLOR(colour), 0.0f, 0.0f },	// bottom right
	};

	DWORD dwSize = sizeof(pVertInd);

	void *pData = nullptr;
	HRESULT hRes = m_pVBSquareIndicator->Map( D3D10_MAP_WRITE_DISCARD, 0, &pData );
	if ( SUCCEEDED(hRes) )
	{
		CopyMemory( pData, pVertInd, dwSize );
		m_pVBSquareIndicator->Unmap( );
	}
}

HRESULT DX10Renderer::CreateIndicatorVB( void )
{
	const TEXMAPVERTEX pVertBorder[] =
	{
		{ D3D10POS(INDICATOR_X+0.5f,				INDICATOR_Y+0.5f,				 0.0f), D3D10COLOR(0xff000000), 0.0f, 0.0f },	// top left
		{ D3D10POS(INDICATOR_X+INDICATOR_Width+0.5, INDICATOR_Y+0.5f,				 0.0f), D3D10COLOR(0xff000000), 0.0f, 0.0f },	// top right
		{ D3D10POS(INDICATOR_X+INDICATOR_Width+0.5,	INDICATOR_Y+INDICATOR_Height+0.5,0.0f), D3D10COLOR(0xff000000), 0.0f, 0.0f },	// bottom right
		{ D3D10POS(INDICATOR_X+0.5f,				INDICATOR_Y+INDICATOR_Height+0.5,0.0f), D3D10COLOR(0xff000000), 0.0f, 0.0f },	// bottom left
		{ D3D10POS(INDICATOR_X+0.5f,				INDICATOR_Y+0.5f,				 0.0f), D3D10COLOR(0xff000000), 0.0f, 0.0f },	// top left
	};

	const TEXMAPVERTEX pVertInd[] = 
	{
		{ D3D10POS(INDICATOR_X+0.5f,				INDICATOR_Y+0.5f,					0.0f), D3D10COLOR(0xffffffff), 0.0f, 0.0f },	// top left
		{ D3D10POS(INDICATOR_X+INDICATOR_Width+0.5,	INDICATOR_Y+0.5f,					0.0f), D3D10COLOR(0xffffffff), 0.0f, 0.0f },	// top right
		{ D3D10POS(INDICATOR_X+0.5f,				INDICATOR_Y+INDICATOR_Height+0.5,	0.0f), D3D10COLOR(0xffffffff), 0.0f, 0.0f },	// bottom left
		{ D3D10POS(INDICATOR_X+INDICATOR_Width+0.5, INDICATOR_Y+INDICATOR_Height+0.5,	0.0f), D3D10COLOR(0xffffffff), 0.0f, 0.0f },	// bottom right
	};
	
	D3D10_BUFFER_DESC borderDesc;
	borderDesc.Usage = D3D10_USAGE_IMMUTABLE;
	borderDesc.ByteWidth = sizeof(pVertBorder);
	borderDesc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
	borderDesc.CPUAccessFlags = 0;
	borderDesc.MiscFlags = 0;

	D3D10_SUBRESOURCE_DATA borderData;
	borderData.pSysMem = pVertBorder;

	D3D10_BUFFER_DESC indDesc;
	indDesc.Usage = D3D10_USAGE_DYNAMIC;
	indDesc.ByteWidth = sizeof(pVertInd);
	indDesc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
	indDesc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
	indDesc.MiscFlags = 0;

	D3D10_SUBRESOURCE_DATA indData;
	indData.pSysMem = pVertInd;
	
	HRESULT hRes;
	hRes = m_pDevice->CreateBuffer( &borderDesc, &borderData, IREF_GETPPTR(m_pVBSquareBorder, ID3D11Buffer) );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "DX10Renderer:CreateIndicatorVB: border buffer creation failed 0x%08x" LOG_CR, hRes );
		return hRes;
	}

	hRes = m_pDevice->CreateBuffer( &indDesc, &indData, IREF_GETPPTR(m_pVBSquareIndicator, ID3D11Buffer) );
	if ( FAILED(hRes) )
	{
		LOG_MSG( "DX10Renderer:CreateIndicatorVB: indicator buffer creation failed 0x%08x" LOG_CR, hRes );
		return hRes;
	}

	return hRes;
}

bool DX10Renderer::InitRenderer( IDXGISwapChain *pSwapChain, IndicatorManager &manager )
{
	#define CHEK(hr, a) if ( FAILED(hr) ) { LOG_MSG( "InitRenderer: %s failed with result 0x%08x" LOG_CR, a, hr ); return false; }
	HRESULT hRes;

	int w = g_Proc.m_Stats.m_SizeWnd.cx, h = g_Proc.m_Stats.m_SizeWnd.cy;

	// set up a rasterizer state we'll use
	D3D10_RASTERIZER_DESC rasterizerState;
    rasterizerState.FillMode = D3D10_FILL_SOLID;
    rasterizerState.CullMode = D3D10_CULL_NONE;
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
	D3D10_DEPTH_STENCIL_DESC dsDesc;
	dsDesc.DepthEnable = false;
	dsDesc.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
	dsDesc.DepthFunc = D3D10_COMPARISON_LESS;
	dsDesc.StencilEnable = true;
	dsDesc.StencilReadMask = D3D10_DEFAULT_STENCIL_READ_MASK;
	dsDesc.StencilWriteMask = D3D10_DEFAULT_STENCIL_WRITE_MASK;
	dsDesc.FrontFace.StencilDepthFailOp = D3D10_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilFailOp = D3D10_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilFunc = D3D10_COMPARISON_ALWAYS;
	dsDesc.FrontFace.StencilPassOp = D3D10_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilDepthFailOp = D3D10_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilFailOp = D3D10_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilFunc = D3D10_COMPARISON_ALWAYS;
	dsDesc.BackFace.StencilPassOp = D3D10_STENCIL_OP_KEEP;
	hRes = m_pDevice->CreateDepthStencilState( &dsDesc, &m_pDepthState);
	CHEK( hRes, "CreateDepthStencilState" );

	// set up indicator textures
	InitIndicatorTextures( manager );

	// vertex buffers should go elsewhere?

	const TEXMAPVERTEX pVertSrc[] = 
	{
		{D3D10POS(0, 0), D3D10COLOR(0xffffffff), 0.0f, 0.0f }, // x, y, z, color, tu, tv
	    {D3D10POS(0, h), D3D10COLOR(0xffffffff), 0.0f, 1.0f },
	    {D3D10POS(w, 0), D3D10COLOR(0xffffffff), 1.0f, 0.0f },
	    {D3D10POS(w, h), D3D10COLOR(0xffffffff), 1.0f, 1.0f },
	};
	
	UINT iSizeSrc = sizeof(pVertSrc);
	
	D3D10_BUFFER_DESC BufferDescription;
	BufferDescription.Usage = D3D10_USAGE_DYNAMIC;
	BufferDescription.ByteWidth = iSizeSrc;
	BufferDescription.BindFlags = D3D10_BIND_VERTEX_BUFFER;
	BufferDescription.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
	BufferDescription.MiscFlags = 0;

	D3D10_SUBRESOURCE_DATA InitialData;
	InitialData.pSysMem = pVertSrc;
	
	hRes = m_pDevice->CreateBuffer(&BufferDescription, &InitialData, IREF_GETPPTR(m_pVBNotification, ID3D11Buffer));
	CHEK( hRes, "CreateBuffer" );

	hRes = CreateIndicatorVB( );
	CHEK( hRes, "CreateIndicatorVB" );

	IRefPtr<ID3DBlob> pVSBlob;
	UINT dwBufferSize = (UINT)strlen(s_DX10TextureShader) + 1;
	DWORD dwShaderFlags = D3D10_SHADER_ENABLE_STRICTNESS;
	s_D3D10CompileShader( s_DX10TextureShader, dwBufferSize, "file", NULL, NULL, "VS", "vs_4_0", dwShaderFlags, IREF_GETPPTR(pVSBlob, ID3DBlob), NULL );
	CHEK( hRes, "D3D10CompileShader (vertex shader)" );

	IRefPtr<ID3DBlob> pPSTexBlob;
	s_D3D10CompileShader( s_DX10TextureShader, dwBufferSize, "file", NULL, NULL, "PS_Tex", "ps_4_0", dwShaderFlags, IREF_GETPPTR(pPSTexBlob, ID3DBlob), NULL );
	CHEK( hRes, "D3D10CompileShader (textured pixel shader)" );

	IRefPtr<ID3DBlob> pPSSolidBlob;
	s_D3D10CompileShader( s_DX10TextureShader, dwBufferSize, "file", NULL, NULL, "PS_Solid", "ps_4_0", dwShaderFlags, IREF_GETPPTR(pPSSolidBlob, ID3DBlob), NULL );
	CHEK( hRes, "D3D10CompileShader (solid pixel shader)" );

	hRes = m_pDevice->CreateVertexShader( pVSBlob->GetBufferPointer( ), pVSBlob->GetBufferSize( ), IREF_GETPPTR(m_pVertexShader, ID3D10VertexShader) );
	CHEK( hRes, "CreateVertexShader" );
	
	hRes = m_pDevice->CreatePixelShader( pPSTexBlob->GetBufferPointer( ), pPSTexBlob->GetBufferSize( ), IREF_GETPPTR(m_pPixelShader, ID3D10PixelShader) );
	CHEK( hRes, "CreatePixelShader (textured)" );

	hRes = m_pDevice->CreatePixelShader( pPSSolidBlob->GetBufferPointer( ), pPSSolidBlob->GetBufferSize( ), IREF_GETPPTR(m_pPixelShaderSolid, ID3D10PixelShader) );
	CHEK( hRes, "CreatePixelShader (solid)" );

	D3D10_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D10_APPEND_ALIGNED_ELEMENT,  D3D10_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D10_APPEND_ALIGNED_ELEMENT,  D3D10_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D10_APPEND_ALIGNED_ELEMENT,  D3D10_INPUT_PER_VERTEX_DATA, 0 },
	};
	UINT numElements = ARRAYSIZE( layout );
	hRes = m_pDevice->CreateInputLayout(layout, numElements, pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), IREF_GETPPTR(m_pVertexLayout, ID3D10InputLayout));
	CHEK( hRes, "CreateInputLayout" );
	
	D3D10_SAMPLER_DESC sampDesc;
    SecureZeroMemory( &sampDesc, sizeof(sampDesc) );
    sampDesc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D10_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D10_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D10_TEXTURE_ADDRESS_WRAP;
    sampDesc.ComparisonFunc = D3D10_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D10_FLOAT32_MAX;
    hRes = m_pDevice->CreateSamplerState( &sampDesc, IREF_GETPPTR(m_pSamplerState, ID3D11SamplerState) );
	CHEK( hRes, "CreateSamplerState" );

	D3D10_BLEND_DESC blendDesc;
	SecureZeroMemory( &blendDesc, sizeof(D3D10_BLEND_DESC) );
	blendDesc.BlendEnable[0] = TRUE;
	blendDesc.SrcBlend = D3D10_BLEND_SRC_ALPHA;
	blendDesc.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
	blendDesc.SrcBlendAlpha = D3D10_BLEND_ONE;
	blendDesc.DestBlendAlpha = D3D10_BLEND_ONE;
	blendDesc.BlendOp = D3D10_BLEND_OP_ADD;
	blendDesc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	blendDesc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
	hRes = m_pDevice->CreateBlendState( &blendDesc, IREF_GETPPTR(m_pBlendStateTextured, ID3D10BlendState) );
	CHEK( hRes, "CreateBlendState" );

	// make a render target view of backbuffer
	IRefPtr<ID3D10Texture2D> pBackBuffer;
	hRes = pSwapChain->GetBuffer( 0, __uuidof(ID3D10Texture2D), IREF_GETPPTRV(pBackBuffer, ID3D10Texture2D) );
	CHEK( hRes, "SwapChain:GetBuffer");

	hRes = m_pDevice->CreateRenderTargetView( pBackBuffer, nullptr, IREF_GETPPTR( m_pRenderTargetView, ID3D10RenderTargetView ) );
	CHEK( hRes, "CreateRenderTargetView" );

	return S_OK;
}

void DX10Renderer::FreeRenderer( void )
{
	m_pVBNotification.ReleaseRefObj( );
	m_pVBSquareIndicator.ReleaseRefObj( );
	m_pVBSquareBorder.ReleaseRefObj( );
	m_pVertexLayout.ReleaseRefObj( );
	m_pVertexShader.ReleaseRefObj( );
	m_pPixelShader.ReleaseRefObj( );
	m_pPixelShaderSolid.ReleaseRefObj( );
	
	m_pSamplerState.ReleaseRefObj( );
	m_pRenderTargetView.ReleaseRefObj( );
	m_pBlendStateTextured.ReleaseRefObj( );

	for( UINT i = 0; i < INDICATE_NONE; i++ )
	{
		m_pIndicatorTexture[i].ReleaseRefObj( );
		m_pResViewNotification[i].ReleaseRefObj( );
	}
}

void DX10Renderer::DrawNewIndicator( IndicatorEvent eIndicatorEvent, BYTE alpha )
{
	if ( !m_pIndicatorTexture[eIndicatorEvent].IsValidRefObj( ) || !m_pResViewNotification[eIndicatorEvent].IsValidRefObj( ) )
		return;

	D3D10_VIEWPORT vp;
	vp.TopLeftX      = 0;
	vp.TopLeftY      = 0;
	vp.Width  = (UINT)( g_Proc.m_Stats.m_SizeWnd.cx );
	vp.Height = (UINT)( g_Proc.m_Stats.m_SizeWnd.cy );
	vp.MinDepth   = 0.0f;
	vp.MaxDepth   = 1.0f;

// save current state
	IRefPtr<ID3D10RasterizerState> pSavedRSState;
	m_pDevice->RSGetState( IREF_GETPPTR(pSavedRSState, ID3D10RasterizerState) );

	IRefPtr<ID3D10DepthStencilState> pSavedDepthState;
	UINT pStencilRef = 0;
	m_pDevice->OMGetDepthStencilState(IREF_GETPPTR(pSavedDepthState, ID3D10DepthStencilState), &pStencilRef);

	// will games with multiple viewports break here?
	D3D10_VIEWPORT originalVP;
	UINT numViewPorts = 1;
	m_pDevice->RSGetViewports(&numViewPorts, &originalVP);

	// save render/depth target so we can restore after
	IRefPtr<ID3D10RenderTargetView> pRenderTargetView;
	IRefPtr<ID3D10DepthStencilView> pDepthStencilView;
	m_pDevice->OMGetRenderTargets(1, IREF_GETPPTR(pRenderTargetView, ID3D10RenderTargetView), IREF_GETPPTR(pDepthStencilView, ID3D10DepthStencilView));

	IRefPtr<ID3D10BlendState> pBlendState;
	float fBlendFactor[4];
	UINT uSampleMask;
	m_pDevice->OMGetBlendState( IREF_GETPPTR(pBlendState, ID3D10BlendState), fBlendFactor, &uSampleMask );

	D3D10_PRIMITIVE_TOPOLOGY old_topology;
	m_pDevice->IAGetPrimitiveTopology( &old_topology );

	IRefPtr<ID3D10InputLayout> pOldInputLayout;
	m_pDevice->IAGetInputLayout( IREF_GETPPTR(pOldInputLayout, ID3D10InputLayout) );	

// setup
	m_pDevice->OMSetRenderTargets(1, m_pRenderTargetView.get_Array(), nullptr );
	m_pDevice->RSSetViewports(1, &vp);						// Set the new viewport for the indicator
	m_pDevice->RSSetState(m_pRasterState);					// Set the new state
	m_pDevice->OMSetDepthStencilState(m_pDepthState, 1);		// Set depth-stencil state to temporarily disable z-buffer

	float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	m_pDevice->OMSetBlendState( m_pBlendStateTextured, blend_factor, 0xffffffff );

	m_pDevice->IASetInputLayout( m_pVertexLayout );
	
	m_pDevice->VSSetShader( m_pVertexShader );
	m_pDevice->PSSetShader( m_pPixelShader );

	m_pDevice->PSSetShaderResources( 0, 1, m_pResViewNotification[eIndicatorEvent].get_Array() );
	m_pDevice->PSSetSamplers( 0, 1, m_pSamplerState.get_Array() );

	UpdateNotificationVB( eIndicatorEvent, alpha );

// draw
	UINT stride = sizeof( TEXMAPVERTEX );
	UINT offset = 0;
	m_pDevice->IASetVertexBuffers( 0, 1, m_pVBNotification.get_Array(), &stride, &offset );
	m_pDevice->IASetPrimitiveTopology( D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
	m_pDevice->Draw( 4, 0 );

// restore state
	m_pDevice->IASetPrimitiveTopology( old_topology );
	m_pDevice->IASetInputLayout( pOldInputLayout.get_RefObj() );	

	m_pDevice->OMSetRenderTargets( 1, pRenderTargetView.get_Array(), pDepthStencilView );

	m_pDevice->RSSetViewports( 1, &originalVP );				// restore the old viewport
	m_pDevice->RSSetState( pSavedRSState );					// restore the old state
	m_pDevice->OMSetDepthStencilState( pSavedDepthState, 1 );	// restore the old depth state (re-enable zbuffer)

	m_pDevice->OMSetBlendState( pBlendState, fBlendFactor, uSampleMask );

	pRenderTargetView.ReleaseRefObj( );
	pOldInputLayout.ReleaseRefObj( );
}

void DX10Renderer::DrawIndicator( TAKSI_INDICATE_TYPE eIndicate )
{
	D3D10_VIEWPORT vp;
	vp.TopLeftX      = 0;
	vp.TopLeftY      = 0;
	vp.Width  = (UINT)( INDICATOR_X*2 + INDICATOR_Width );
	vp.Height = (UINT)( INDICATOR_Y*2 + INDICATOR_Height );
	vp.MinDepth   = 0.0f;
	vp.MaxDepth   = 1.0f;

// save current state
	IRefPtr<ID3D10RasterizerState> pSavedRSState;
	m_pDevice->RSGetState( IREF_GETPPTR(pSavedRSState, ID3D10RasterizerState) );

	IRefPtr<ID3D10DepthStencilState> pSavedDepthState;
	UINT pStencilRef = 0;
	m_pDevice->OMGetDepthStencilState( IREF_GETPPTR(pSavedDepthState, ID3D10DepthStencilState), &pStencilRef );

	// will games with multiple viewports break here?
	D3D10_VIEWPORT originalVP;
	UINT numViewPorts = 1;
	m_pDevice->RSGetViewports(&numViewPorts, &originalVP);

	// save render/depth target so we can restore after
	IRefPtr<ID3D10RenderTargetView> pRenderTargetView;
	IRefPtr<ID3D10DepthStencilView> pDepthStencilView;
	m_pDevice->OMGetRenderTargets( 1, IREF_GETPPTR(pRenderTargetView, ID3D10RenderTargetView), IREF_GETPPTR(pDepthStencilView, ID3D10DepthStencilView) );

	IRefPtr<ID3D10BlendState> pBlendState;
	float fBlendFactor[4];
	UINT uSampleMask;
	m_pDevice->OMGetBlendState( IREF_GETPPTR(pBlendState, ID3D10BlendState), fBlendFactor, &uSampleMask );

	D3D10_PRIMITIVE_TOPOLOGY old_topology;
	m_pDevice->IAGetPrimitiveTopology( &old_topology );

	IRefPtr<ID3D10InputLayout> pOldInputLayout;
	m_pDevice->IAGetInputLayout( IREF_GETPPTR(pOldInputLayout, ID3D10InputLayout) );	

// setup
	m_pDevice->OMSetRenderTargets( 1, IREF_GETPPTR(m_pRenderTargetView, ID3D10RenderTargetView), nullptr );
	m_pDevice->RSSetViewports( 1, &vp );						// Set the new viewport for the indicator
	m_pDevice->RSSetState( m_pRasterState );					// Set the new state
	m_pDevice->OMSetDepthStencilState( m_pDepthState, 1 );		// Set depth-stencil state to temporarily disable z-buffer

	float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	m_pDevice->OMSetBlendState( m_pBlendStateTextured, blend_factor, 0xffffffff );

	m_pDevice->IASetInputLayout( m_pVertexLayout );
	
	m_pDevice->VSSetShader( m_pVertexShader );
	m_pDevice->PSSetShader( m_pPixelShaderSolid );

	UpdateSquareIndicatorVB( eIndicate );

// draw
	UINT stride = sizeof( TEXMAPVERTEX );
	UINT offset = 0;
	m_pDevice->IASetVertexBuffers( 0, 1, IREF_GETPPTR(m_pVBSquareIndicator, ID3D11Buffer), &stride, &offset );
	m_pDevice->IASetPrimitiveTopology( D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
	m_pDevice->Draw( 4, 0 );

	m_pDevice->IASetVertexBuffers( 0, 1, IREF_GETPPTR(m_pVBSquareBorder, ID3D11Buffer), &stride, &offset );
	m_pDevice->IASetPrimitiveTopology( D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP );
	m_pDevice->Draw( 5, 0 );


// restore state
	m_pDevice->IASetPrimitiveTopology( old_topology );
	m_pDevice->IASetInputLayout( pOldInputLayout );	

	m_pDevice->OMSetRenderTargets( 1, IREF_GETPPTR(pRenderTargetView, ID3D10RenderTargetView), pDepthStencilView );

	m_pDevice->RSSetViewports( 1, &originalVP );				// restore the old viewport
	m_pDevice->RSSetState( pSavedRSState );					// restore the old state
	m_pDevice->OMSetDepthStencilState( pSavedDepthState, 1 );	// restore the old depth state (re-enable zbuffer)

	m_pDevice->OMSetBlendState( pBlendState, fBlendFactor, uSampleMask );

	pRenderTargetView.ReleaseRefObj( );
	pOldInputLayout.ReleaseRefObj( );
}


using namespace std;
static unique_ptr<DX10Renderer> renderer;

static DX10Renderer *get_renderer(IDXGISwapChain *swap)
{
	if (!renderer)
	{
		ID3D10Device *dev = nullptr;
		auto hr = swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev);
		if (FAILED(hr) || !dev)
			return nullptr;

		DXGI_SWAP_CHAIN_DESC desc;
		hr = swap->GetDesc(&desc);
		if (FAILED(hr))
			return nullptr;

		g_Proc.m_Stats.m_SizeWnd.cx = desc.BufferDesc.Width;
		g_Proc.m_Stats.m_SizeWnd.cy = desc.BufferDesc.Height;

		D3D10_LoadFunctions();

		renderer.reset(new DX10Renderer{ dev }); //release dev?
		renderer->InitRenderer(swap, indicatorManager);
	}

	return renderer.get();
}


void overlay_d3d10_free()
{
	renderer.reset();
}

C_EXPORT void overlay_draw_d3d10(IDXGISwapChain *swap)
{
	auto renderer = get_renderer(swap);
	if (!renderer)
		return;

	renderer->DrawNewIndicator(INDICATE_CAPTURING, 255);
}