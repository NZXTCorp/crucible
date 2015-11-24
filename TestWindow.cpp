// [TestWindow.h 2015-11-06 abright]
// test window for libobs rendering

#include <windows.h>

#include "TestWindow.h"

ATOM TestWindow::RegisterWindowClass(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;

	SecureZeroMemory(&wcex, sizeof(WNDCLASSEX));
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = 0;
	wcex.lpfnWndProc = &TestWindow::WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = NULL;
	wcex.hCursor = LoadCursor(NULL, IDC_HAND);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = "TestWindowClass";
	wcex.hIconSm = NULL;

	return RegisterClassEx(&wcex);
}

LRESULT CALLBACK TestWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	TestWindow* window = nullptr;

	// first call, grab the pointer from create params and call its member wndproc (this way we get create and nccreate)
	/*
	if (message == WM_NCCREATE)
	{
		CREATESTRUCT *cr = (CREATESTRUCT *)lParam;
		if (cr)
		{
			window = reinterpret_cast<TestWindow *>(cr->lpCreateParams);
			if (window)
				return window->WindowProc(message, wParam, lParam);
		}
	}*/

	// normal operation, we'll just grab it from the usual spot and do our thing
	window = reinterpret_cast<TestWindow *>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
	if (!window)
		return DefWindowProc(hWnd, message, wParam, lParam);

	return window->WindowProc(message, wParam, lParam);
}

TestWindow::TestWindow(HINSTANCE hInstance) : m_hInst(hInstance), m_hWnd(nullptr), m_width(0), m_height(0)
{
}

TestWindow::~TestWindow(void)
{
	Destroy();
}

bool TestWindow::Create(int width, int height, const char *title)
{
	int left = 0;
	int top = 0;

	m_width = width;
	m_height = height;

	NONCLIENTMETRICS ncm;
	SecureZeroMemory(&ncm, sizeof(NONCLIENTMETRICS));
	ncm.cbSize = sizeof(NONCLIENTMETRICS);
	SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &ncm, 0);

	// bump up the window size to include non-client area (borders+caption) - the width and height we're called with will be the client area size
	m_width += (ncm.iPaddedBorderWidth * 2);
	m_height += (ncm.iPaddedBorderWidth * 2) + ncm.iCaptionHeight;

	left = (GetSystemMetrics(SM_CXSCREEN) / 2) - (m_width / 2);
	top = (GetSystemMetrics(SM_CYSCREEN) / 2) - (m_height / 2);

	m_hWnd = ::CreateWindowEx(0, "TestWindowClass", title, WS_OVERLAPPEDWINDOW, left, top, m_width, m_height, 0, 0, m_hInst, (LPVOID)this);
	if (!m_hWnd)
		return false;

	SetWindowLongPtr(m_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

	return true;
}

void TestWindow::Destroy(void)
{
	SetWindowLongPtr(m_hWnd, GWLP_USERDATA, 0);
	::DestroyWindow(m_hWnd);
}

void TestWindow::Show(void)
{
	ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
}

LRESULT TestWindow::WindowProc(UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT result = 0;

	if (message == WM_CLOSE)
		PostQuitMessage(0);

	return DefWindowProc(m_hWnd, message, wParam, lParam);
}
