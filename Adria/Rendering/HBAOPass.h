#pragma once
#include "BlurPass.h"
#include "Graphics/GfxDescriptor.h"
#include "RenderGraph/RenderGraphResourceId.h"


namespace adria
{
	class RenderGraph;
	class GfxDevice;
	class GfxTexture;

	class HBAOPass
	{
		struct HBAOParams
		{
			Float   hbao_power = 6.0f;
			Float   hbao_radius = 3.0f;
		};

	public:
		static constexpr Uint32 NOISE_DIM = 8;

	public:
		HBAOPass(GfxDevice* gfx, Uint32 w, Uint32 h);
		~HBAOPass();

		void AddPass(RenderGraph& rendergraph);
		void OnResize(Uint32 w, Uint32 h);
		void OnSceneInitialized();
		void GUI();

	private:
		GfxDevice* gfx;
		Uint32 width, height;
		HBAOParams params{};
		std::unique_ptr<GfxTexture> hbao_random_texture;
		GfxDescriptor hbao_random_texture_srv;
		BlurPass blur_pass;
		std::unique_ptr<GfxComputePipelineState> hbao_pso;

	private:
		void CreatePSO();
	};

}