#pragma once
#include "PostEffect.h"
#include "Graphics/GfxPipelineStateFwd.h"
#include "entt/fwd.hpp"

namespace adria
{
	class GfxDevice;
	class RenderGraph;

	class SunPass : public PostEffect
	{
	public:
		SunPass(GfxDevice* gfx, Uint32 width, Uint32 height);

		virtual Bool IsEnabled(PostProcessor const*) const override
		{
			return true;
		}
		virtual void AddPass(RenderGraph&, PostProcessor*) override;

		virtual void OnResize(Uint32 w, Uint32 h) override
		{
			width = w, height = h;
		}

	private:
		GfxDevice* gfx;
		Uint32 width;
		Uint32 height;
		std::unique_ptr<GfxGraphicsPipelineState> sun_pso;

	private:
		void CreatePSO();
	};
}