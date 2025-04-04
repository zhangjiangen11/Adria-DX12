#pragma once
#include "OpenImageDenoise/oidn.h"
#include "Graphics/GfxFence.h"

namespace adria
{
	class GfxDevice;
	class GfxBuffer;
	class GfxTexture;
	class GfxCommandList;
	class RenderGraph;

	class OIDNDenoiserPass 
	{
	public:
		explicit OIDNDenoiserPass(GfxDevice* gfx);
		~OIDNDenoiserPass();

		Bool IsSupported() const { return supported; }
		void AddPass(RenderGraph& rendergraph);
		void Reset();

	private:
		GfxDevice* gfx;
		OIDNDevice oidn_device = nullptr;
		OIDNFilter oidn_filter = nullptr;
		OIDNBuffer oidn_color_buffer = nullptr;
		OIDNBuffer oidn_albedo_buffer = nullptr;
		OIDNBuffer oidn_normal_buffer = nullptr;
		GfxFence oidn_fence;
		Uint64 oidn_fence_value = 0;

		std::unique_ptr<GfxBuffer> color_buffer;
		std::unique_ptr<GfxBuffer> albedo_buffer;
		std::unique_ptr<GfxBuffer> normal_buffer;
		Bool denoised = false;
		Bool supported = false;

	private:
		void CreateBuffers(GfxTexture const& color_texture, GfxTexture const& albedo_texture, GfxTexture const& normal_texture);
		void ReleaseBuffers();

		void Denoise(GfxCommandList* cmd_list, GfxTexture const& color_texture, GfxTexture const& albedo_texture, GfxTexture const& normal_texture);
	};
}