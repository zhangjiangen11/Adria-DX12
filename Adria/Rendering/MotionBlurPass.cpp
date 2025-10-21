#include "MotionBlurPass.h"
#include "ShaderStructs.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "Postprocessor.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "Core/ConsoleManager.h"
#include "Editor/GUICommand.h"

namespace adria
{
	static TAutoConsoleVariable<Bool> MotionBlur("r.MotionBlur", false, "Enable or Disable Motion Blur");

	MotionBlurPass::MotionBlurPass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h) 
	{
		CreatePSO();
	}

	void MotionBlurPass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		RGResourceName last_resource = postprocessor->GetFinalResource();
		struct MotionBlurPassData
		{
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId velocity;
			RGTextureReadWriteId output;
		};
		rg.AddPass<MotionBlurPassData>("Motion Blur Pass",
			[=](MotionBlurPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc motion_blur_desc{};
				motion_blur_desc.width = width;
				motion_blur_desc.height = height;
				motion_blur_desc.format = GfxFormat::R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_NAME(MotionBlurOutput), motion_blur_desc);
				data.output = builder.WriteTexture(RG_NAME(MotionBlurOutput));
				data.input = builder.ReadTexture(last_resource, ReadAccess_NonPixelShader);
				data.velocity = builder.ReadTexture(RG_NAME(VelocityBuffer), ReadAccess_NonPixelShader);
			},
			[=](MotionBlurPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.input),
					ctx.GetReadOnlyTexture(data.velocity),
					ctx.GetReadWriteTexture(data.output)
				};
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_descriptors);

				struct MotionBlurConstants
				{
					Uint32 scene_idx;
					Uint32 velocity_idx;
					Uint32 output_idx;
				} constants =
				{
					.scene_idx = table, .velocity_idx = table + 1, .output_idx = table + 2
				};

				cmd_list->SetPipelineState(motion_blur_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
			}, RGPassType::Compute, RGPassFlags::None);

		postprocessor->SetFinalResource(RG_NAME(MotionBlurOutput));
	}

	void MotionBlurPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	Bool MotionBlurPass::IsEnabled(PostProcessor const*) const
	{
		return MotionBlur.Get();
	}

	void MotionBlurPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNode("Motion Blur"))
				{
					ImGui::Checkbox("Enable", MotionBlur.GetPtr());
					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing);
	}

	void MotionBlurPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_MotionBlur;
		motion_blur_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

}

