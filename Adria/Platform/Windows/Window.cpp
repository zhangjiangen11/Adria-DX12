#include "Platform/Window.h"
#include "Utilities/StringConversions.h"
#include "resource.h"

namespace adria
{
	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
	{
		Window* this_window = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		WindowEventInfo window_data{};
		window_data.handle = hwnd;
		window_data.msg  = static_cast<Uint32>(msg);
		window_data.wparam = static_cast<Uint64>(w_param);
		window_data.lparam = static_cast<Int64>(l_param);
		window_data.width = this_window ? static_cast<Float>(this_window->Width()) : 0.0f;
		window_data.height = this_window ? static_cast<Float>(this_window->Height()) : 0.0f;

		LRESULT result = 0ll;
		if (msg == WM_CLOSE || msg == WM_DESTROY)
		{
			PostQuitMessage(0);
			return 0;
		}
		else if (msg == WM_DISPLAYCHANGE || msg == WM_SIZE)
		{
			window_data.width = static_cast<Float>(l_param & 0xffff);
			window_data.height = static_cast<Float>((l_param >> 16) & 0xffff);
		}
		else result = DefWindowProc(hwnd, msg, w_param, l_param);
        if (this_window) this_window->BroadcastEvent(window_data);
		return result;
	}


    Window::Window(WindowCreationParams const& init)
    {
        HINSTANCE hinstance = GetModuleHandle(NULL);
        const std::wstring window_title = ToWideString(init.title);
		const LPCWSTR class_name = L"AdriaClass";
		LONG  window_width = (LONG)init.width;
        LONG  window_height = (LONG)init.height;

        WNDCLASSEX wcex{};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = hinstance;
        wcex.hIcon = LoadIcon(hinstance, MAKEINTRESOURCE(IDI_ICON1));
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = (HBRUSH)(COLOR_BACKGROUND + 2);
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = class_name;
        wcex.hIconSm = nullptr;

		RECT rect = { 0, 0, window_width, window_height };
		AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        window_width = rect.right - rect.left;
        window_height = rect.bottom - rect.top;

        if (!RegisterClassExW(&wcex))
        {
            MessageBoxA(nullptr, "Window class registration failed!", "Fatal Error!", MB_ICONEXCLAMATION | MB_OK);
            return;
        }

        HWND hwnd = CreateWindowExW
        (
            0, class_name,
            window_title.c_str(),
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            window_width, window_height,
            nullptr, nullptr, hinstance, nullptr
        );

        if (!hwnd)
        {
            MessageBox(nullptr, L"Window creation failed!", L"Fatal Error!", MB_ICONEXCLAMATION | MB_OK);
            return;
        }

		window_handle = hwnd;

		SetWindowLong(hwnd, GWL_STYLE, GetWindowLong(hwnd, GWL_STYLE) & ~WS_MINIMIZEBOX);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        if (init.maximize)
        {
            ShowWindow(hwnd, SW_SHOWMAXIMIZED);
        }
        else
        {
            ShowWindow(hwnd, SW_SHOWNORMAL);
        }

		UpdateWindow(hwnd);
		SetFocus(hwnd);
    }

	Window::~Window()
	{
		HWND hwnd = static_cast<HWND>(window_handle);
        if (hwnd)
        {
            DestroyWindow(hwnd);
        }
	}

	Uint32 Window::Width() const
	{
		HWND hwnd = static_cast<HWND>(window_handle);
		RECT rect{};
        GetClientRect(hwnd, &rect);
		return static_cast<Uint32>(rect.right - rect.left);
	}
    Uint32 Window::Height() const
    {
		HWND hwnd = static_cast<HWND>(window_handle);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        return static_cast<Uint32>(rect.bottom - rect.top);
    }
    Uint32 Window::PositionX() const
    {
		HWND hwnd = static_cast<HWND>(window_handle);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        ClientToScreen(hwnd, (LPPOINT)&rect.left);
        ClientToScreen(hwnd, (LPPOINT)&rect.right);
        return rect.left;
    }
	Uint32 Window::PositionY() const
	{
		HWND hwnd = static_cast<HWND>(window_handle);
		RECT rect{};
		GetClientRect(hwnd, &rect);
		ClientToScreen(hwnd, (LPPOINT)&rect.left);
		ClientToScreen(hwnd, (LPPOINT)&rect.right);
		return rect.top;
	}

	Bool Window::Loop()
    {
        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) return false;
        }
        return true;
    }

	void Window::Quit(Int32 exit_code)
	{
        PostQuitMessage(exit_code);
	}

    void* Window::Handle() const
    {
        return window_handle;
    }
    Bool Window::IsActive() const
    {
		HWND hwnd = static_cast<HWND>(window_handle);
        return GetForegroundWindow() == hwnd;
    }

	void Window::BroadcastEvent(WindowEventInfo const& data)
	{
        window_event.Broadcast(data);
	}

}
