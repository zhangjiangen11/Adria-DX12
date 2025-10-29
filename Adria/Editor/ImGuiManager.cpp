#include "ImGuiManager.h"
#include "Graphics/GfxDevice.h"
#if defined(ADRIA_PLATFORM_WINDOWS)
#include "D3D12/D3D12ImGuiManager.h"
#endif

namespace adria
{
	std::unique_ptr<ImGuiManager> CreateImguiManager(GfxDevice* gfx)
	{
#if defined(ADRIA_PLATFORM_WINDOWS)
		if (gfx->GetBackend() == GfxBackend::D3D12)
		{
			return std::make_unique<D3D12ImGuiManager>(gfx);
		}
#endif
		ADRIA_ASSERT_MSG(false, "No ImGuiManager implementation for this backend!");
		return nullptr;
	}
}
