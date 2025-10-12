#pragma once
#include "PostEffect.h"
#include "Graphics/GfxPipelineStateFwd.h"

namespace adria
{
	class GfxDevice;
	class RenderGraph;

	class MotionVectorsPass : public PostEffect
	{
	public:
		MotionVectorsPass(GfxDevice* gfx, Uint32 w, Uint32 h);

		virtual Bool IsEnabled(PostProcessor const*) const override;
		virtual void AddPass(RenderGraph& rg, PostProcessor*) override;
		virtual void OnResize(Uint32 w, Uint32 h) override;

	private:
		GfxDevice* gfx;
		Uint32 width, height;
		std::unique_ptr<GfxComputePipelineState> motion_vectors_pso;

	private:
		void CreatePSO();
	};
}