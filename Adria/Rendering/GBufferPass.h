#pragma once
#include "Graphics/GfxPipelineStatePermutations.h"
#include "RenderGraph/RenderGraphResourceId.h"
#include "entt/entity/fwd.hpp"

namespace adria
{
	class GfxDevice;
	class GfxCommandList;
	class RenderGraph;
	enum class RendererDebugView : Uint32;

	class GBufferPass
	{
	public:
		GBufferPass(entt::registry& reg, GfxDevice* gfx, Uint32 w, Uint32 h);
		~GBufferPass();

		void AddPass(RenderGraph& rendergraph);
		void OnResize(Uint32 w, Uint32 h);
		void OnRainEvent(Bool enabled)
		{
			raining = enabled;
		}
		void OnDebugViewChanged(RendererDebugView renderer_output);
		void OnTransparentChanged(Bool skip)
		{
			skip_alpha_blended = skip;
		}

	private:
		entt::registry& reg;
		GfxDevice* gfx;
		Uint32 width, height;
		Bool raining = false;
		Bool debug_mipmaps = false;
		Bool triangle_overdraw = false;
		Bool material_ids = false;
		Bool skip_alpha_blended = false;
		std::unique_ptr<GfxGraphicsPipelineStatePermutations> gbuffer_psos;

	private:
		void CreatePSOs();
		template<typename View>
		void ProcessBatches(View view, GfxCommandList* cmd_list);
	};
}