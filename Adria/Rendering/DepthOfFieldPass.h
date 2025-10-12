#pragma once
#include "PostEffect.h"
#include "BlurPass.h"
#include "Graphics/GfxPipelineStatePermutations.h"

namespace adria
{
	class GfxDevice;
	class GfxTexture;
	class GfxComputePipelineState;
	class RenderGraph;

	class DepthOfFieldPass : public PostEffect
	{
	public:
		DepthOfFieldPass(GfxDevice* gfx, Uint32 w, Uint32 h);
		~DepthOfFieldPass();

		virtual void AddPass(RenderGraph&, PostProcessor*) override;
		virtual void OnResize(Uint32, Uint32) override;
		virtual Bool IsEnabled(PostProcessor const*) const override;
		virtual void OnSceneInitialized() override;
		virtual void GUI();

	private:
		GfxDevice* gfx;
		Uint32 width, height;
		BlurPass blur_pass;

		std::unique_ptr<GfxManagedComputePipelineState> compute_coc_pso;
		std::unique_ptr<GfxManagedComputePipelineState> compute_separated_coc_pso;
		std::unique_ptr<GfxManagedComputePipelineState> downsample_coc_pso;
		std::unique_ptr<GfxManagedComputePipelineState> compute_prefiltered_texture_pso;
		std::unique_ptr<GfxComputePipelineStatePermutations> bokeh_first_pass_psos;
		std::unique_ptr<GfxComputePipelineStatePermutations> bokeh_second_pass_psos;
		std::unique_ptr<GfxManagedComputePipelineState> compute_posfiltered_texture_pso;
		std::unique_ptr<GfxManagedComputePipelineState> combine_pso;

		std::unique_ptr<GfxTexture> bokeh_large_kernel;
		std::unique_ptr<GfxTexture> bokeh_small_kernel;

	private:
		void CreatePSOs();
		void CreateSmallBokehKernel();
		void CreateLargeBokehKernel();

		void AddComputeCircleOfConfusionPass(RenderGraph&);
		void AddSeparatedCircleOfConfusionPass(RenderGraph&);
		void AddDownsampleCircleOfConfusionPass(RenderGraph&);
		void AddComputePrefilteredTexturePass(RenderGraph&, RGResourceName);
		void AddBokehFirstPass(RenderGraph&, RGResourceName);
		void AddBokehSecondPass(RenderGraph&);
		void AddComputePostfilteredTexturePass(RenderGraph&);
		void AddCombinePass(RenderGraph&, RGResourceName);
	};

}