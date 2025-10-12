#include "MotionVectorsPass.h"
#include "ShaderStructs.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "Postprocessor.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"

namespace adria
{
	MotionVectorsPass::MotionVectorsPass(GfxDevice* gfx, Uint32 w, Uint32 h)
		: gfx(gfx), width(w), height(h)
	{
		CreatePSO();
	}

	Bool MotionVectorsPass::IsEnabled(PostProcessor const* postprocessor) const
	{
		return postprocessor->NeedsVelocityBuffer();
	}

	void MotionVectorsPass::AddPass(RenderGraph& rg, PostProcessor*)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		struct MotionVectorsPassData
		{
			RGTextureReadOnlyId depth;
			RGTextureReadWriteId velocity;
		};
		rg.AddPass<MotionVectorsPassData>("Velocity Buffer Pass",
			[=](MotionVectorsPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc velocity_buffer_desc{};
				velocity_buffer_desc.width = width;
				velocity_buffer_desc.height = height;
				velocity_buffer_desc.format = GfxFormat::R16G16_FLOAT;

				builder.DeclareTexture(RG_NAME(VelocityBuffer), velocity_buffer_desc);
				data.velocity = builder.WriteTexture(RG_NAME(VelocityBuffer));
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[=](MotionVectorsPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth),
					ctx.GetReadWriteTexture(data.velocity)
				};
				GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_descriptors));
				gfx->CopyDescriptors(dst_descriptor, src_descriptors);
				Uint32 const i = dst_descriptor.GetIndex();

				struct MotionVectorsConstants
				{
					Uint32   depth_idx;
					Uint32   output_idx;
				} constants =
				{
					.depth_idx = i, .output_idx = i + 1
				};

				cmd_list->SetPipelineState(motion_vectors_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
			}, RGPassType::Compute, RGPassFlags::None);
	}

	void MotionVectorsPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	void MotionVectorsPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_MotionVectors;
		motion_vectors_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

}

