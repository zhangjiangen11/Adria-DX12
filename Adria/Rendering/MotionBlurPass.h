#pragma once
#include "PostEffect.h"
#include "Graphics/GfxPipelineStateFwd.h"

namespace adria
{
	class GfxDevice;
	class RenderGraph;

	class MotionBlurPass: public PostEffect
	{
	public:
		MotionBlurPass(GfxDevice* gfx, Uint32 w, Uint32 h);

		virtual void AddPass(RenderGraph&, PostProcessor*) override;
		virtual void OnResize(Uint32, Uint32) override;
		virtual Bool IsEnabled(PostProcessor const*) const override;
		virtual void GUI() override;

	private:
		GfxDevice* gfx;
		Uint32 width, height;
		std::unique_ptr<GfxComputePipelineState> motion_blur_pso;

	private:
		void CreatePSO();
	};

}