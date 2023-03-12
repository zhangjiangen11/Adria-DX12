#pragma once
#include <d3d12.h>
#include "../Utilities/AutoRefCountPtr.h"
#include "../Core/Definitions.h"

namespace adria
{
	class GfxDevice;

	class GfxFence
	{
	public:
		GfxFence();
		~GfxFence();

		bool Create(GfxDevice* gfx, char const* name);

		void Wait(uint64 value);
		void Signal(uint64 value);

		bool IsCompleted(uint64 value);
		uint64 GetCompletedValue() const;

		operator ID3D12Fence* () const { return fence.Get(); }
	private:
		ArcPtr<ID3D12Fence> fence = nullptr;
		HANDLE event = nullptr;
	};
}