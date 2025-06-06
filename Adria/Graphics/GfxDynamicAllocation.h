#pragma once
#include "Utilities/AllocatorUtil.h"

namespace adria
{
	class GfxBuffer;

	struct GfxDynamicAllocation
	{
		GfxBuffer* buffer = nullptr;
		void* cpu_address = nullptr;
		Uint64 gpu_address = 0;
		Uint64 offset = 0;
		Uint64 size = 0;

		void Update(void const* data, Uint64 size, Uint64 offset = 0)
		{
			memcpy((Uint8*)cpu_address + offset, data, size);
		}

		template<typename T>
		void Update(T const& data)
		{
			memcpy(cpu_address, &data, sizeof(T));
		}
	};
}