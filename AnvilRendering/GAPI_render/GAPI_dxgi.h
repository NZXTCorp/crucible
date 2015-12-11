// [GAPI_dxgi.h 2014-09-01 abright]
// new DXGI graphics api handler, to hook dxgi swapchain and hand off to appropriate d3d handler
// D3D 10 and up all use the same DXGI swap chain code and cant hook individually
// we'll do the main dxgi swapchain hooking here and hand off calls to dx10/11 modules as needed

#ifndef GAPI_DXGI_NASTY
#define GAPI_DXGI_NASTY

interface IDXGISwapChain;
class IndicatorManager;

// smaller class for dxgi-dependent graphics apis. they do their own capture and rendering, but no hooking because dxgi handles it
class DXGIClient
{
protected:
	IDXGISwapChain *m_pSwapChain;
	DXGI_SWAP_CHAIN_DESC m_desc;
public:
	DXGIClient( IDXGISwapChain *pSwapChain );
	~DXGIClient( void );

	// setup/free shared texture stuff for capture
	virtual HANDLE PrepareCapture( void ) = 0; // return handle of shared texture or null if error
	virtual void FinishCapture( void ) = 0;

	// copy a frame from backbuffer to the crucible shared texture
	virtual HRESULT GetFrame( void ) = 0;
	
	// setup/free indicator/overlay rendering
	virtual bool InitRenderer( IndicatorManager &manager ) = 0;
	virtual void FreeRenderer( void ) = 0;

	virtual void DrawNewIndicator( IndicatorEvent eIndicatorEvent, BYTE alpha ) = 0;
	virtual void DrawIndicator( TAKSI_INDICATE_TYPE eIndicate ) = 0;
};
	

struct CTaksiGAPI_DXGI : public CTaksiGAPIBase
{
public:
	CTaksiGAPI_DXGI( void );
	~CTaksiGAPI_DXGI( void );

	virtual const TCHAR* get_DLLName( void ) const
	{
		return TEXT("dxgi.dll");
	}
	virtual TAKSI_GAPI_TYPE get_GAPIType( void ) const
	{
		return TAKSI_GAPI_DXGI;
	}

	virtual bool FindHookOffsets( HWND hWnd );

	virtual HRESULT HookFunctions( void );
	virtual void UnhookFunctions( void );
	virtual void FreeDll( void );

	virtual HWND GetFrameInfo( SIZE& rSize );
	virtual HRESULT GetFrame( void );

	// check the d3d device associated with swap chain and set m_pRealGAPI to something appropriate
	bool SelectGAPI( IDXGISwapChain *pSwapChain );

	void HandleReset( IDXGISwapChain *pSwapChain );

	virtual HRESULT PrepareCapture( void );
	virtual void FinishCapture( void );

	virtual void RenderFrame( void );
	virtual bool InitRenderer( void );
	virtual void FreeRenderer( void );

	void CheckHooks( void );
public:
	bool m_bMultisampled;
	bool m_bWindowed; // running in a windowed mode?
	IDXGISwapChain* m_pSwapChain;
	DXGIClient *m_pClientAPI; // the actual graphics api in use - d3d 10, 11, whatever comes next...

	UINT_PTR m_nDXGI_SCPresent;
	UINT_PTR m_nDXGI_SCRelease;
	UINT_PTR m_nDXGI_SCResizeBuffers;
};
extern CTaksiGAPI_DXGI g_DXGI;

#endif