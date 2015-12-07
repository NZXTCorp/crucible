// [TestWindow.h 2015-11-06 abright]
// test window for libobs rendering

#ifndef TEST_WINDOW_NASTY
#define TEST_WINDOW_NASTY

class TestWindow
{
private:
	static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

	LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam);

	HINSTANCE m_hInst;
	HWND m_hWnd;

	int m_width;
	int m_height;
public:
	static ATOM RegisterWindowClass(HINSTANCE hInstance);

	TestWindow(HINSTANCE hInstance);
	~TestWindow(void);

	// create a the window. width and height are desired client area size (not including borders, which are handled automatically)
	bool Create(int width, int height, const char *title);
	// destroy the window
	void Destroy(void);

	// show the window
	void Show(void);

	HWND GetHandle(void) { return m_hWnd; }

	int GetWidth(void) { return m_width; }
	int GetHeight(void) { return m_height; }
};

#endif