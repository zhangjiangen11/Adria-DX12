#pragma once
#include "Utilities/Delegate.h"

namespace adria
{
	using WindowHandle = void*;

	struct WindowEventInfo
	{
		void*  handle	= nullptr;
		Uint32 msg		= 0;
        Uint64 wparam	= 0;
        Int64  lparam	= 0;
		Float  width	= 0.0f;
		Float  height	= 0.0f;
	};

    struct WindowCreationParams
    {
        Char const* title;
        Uint32 width, height;
        Bool maximize;
    };

	DECLARE_EVENT(WindowEvent, Window, WindowEventInfo const&)

	class Window
	{
	public:
		Window(WindowCreationParams const& init);
		~Window();

		Uint32 Width() const;
		Uint32 Height() const;

		Uint32 PositionX() const;
		Uint32 PositionY() const;

		Bool Loop();
		void Quit(Int32 exit_code);

		void* Handle() const;
		Bool  IsActive() const;

		WindowEvent& GetWindowEvent() { return window_event; }
		void BroadcastEvent(WindowEventInfo const&);

	private:
		WindowHandle window_handle = nullptr;
		WindowEvent window_event;
	};
}