#include "SSRPass.h"
#include "ShaderStructs.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "Postprocessor.h" 
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"
#include "Core/ConsoleManager.h"

namespace adria
{
	static TAutoConsoleVariable<Bool>	SSR("r.SSR", true, "0 - Disabled, 1 - Enabled");
	static TAutoConsoleVariable<Float>	SSRRayStep("r.SSR.RayStep", 1.60f, "Ray Step in SSR Ray March");
	static TAutoConsoleVariable<Float>  SSRRayHitThreshold("r.SSR.HitThreshold", 2.0f, "Ray Hit Threshold in SSR Ray March");

	SSRPass::SSRPass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h)
	{
		CreatePSO();
	}

	Bool SSRPass::IsEnabled(PostProcessor const*) const
	{
		return SSR.Get();
	}

	void SSRPass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		RGResourceName last_resource = postprocessor->GetFinalResource();
		struct SSRPassData
		{
			RGTextureReadOnlyId normals;
			RGTextureReadOnlyId roughness;
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId depth;
			RGTextureReadWriteId output;
		};

		rg.AddPass<SSRPassData>("SSR Pass",
			[=](SSRPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc ssr_output_desc{};
				ssr_output_desc.width = width;
				ssr_output_desc.height = height;
				ssr_output_desc.format = GfxFormat::R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_NAME(SSR_Output), ssr_output_desc);
				data.output = builder.WriteTexture(RG_NAME(SSR_Output));
				data.input = builder.ReadTexture(last_resource, ReadAccess_NonPixelShader);
				data.normals = builder.ReadTexture(RG_NAME(GBufferNormal), ReadAccess_NonPixelShader);
				data.roughness = builder.ReadTexture(RG_NAME(GBufferAlbedo), ReadAccess_NonPixelShader);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[=](SSRPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth),
					ctx.GetReadOnlyTexture(data.normals),
					ctx.GetReadOnlyTexture(data.roughness),
					ctx.GetReadOnlyTexture(data.input),
					ctx.GetReadWriteTexture(data.output)
				};
				GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_descriptors));
				gfx->CopyDescriptors(dst_descriptor, src_descriptors);
				Uint32 const i = dst_descriptor.GetIndex();

				struct SSRConstants
				{
					Float ssr_ray_step;
					Float ssr_ray_hit_threshold;

					Uint32 depth_idx;
					Uint32 normal_idx;
					Uint32 diffuse_idx;
					Uint32 scene_idx;
					Uint32 output_idx;
				} constants =
				{
					.ssr_ray_step = SSRRayStep.Get(), .ssr_ray_hit_threshold = SSRRayHitThreshold.Get(),
					.depth_idx = i, .normal_idx = i + 1, .diffuse_idx = i + 2, .scene_idx = i + 3, .output_idx = i + 4
				};

				cmd_list->SetPipelineState(ssr_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
			}, RGPassType::Compute);

		postprocessor->SetFinalResource(RG_NAME(SSR_Output));
	}

	void SSRPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	void SSRPass::GUI()
	{
		QueueGUI([&]() {
			if (ImGui::TreeNodeEx("Screen-Space Reflections", 0))
			{
				ImGui::Checkbox("Enable SSR", SSR.GetPtr());
				if (SSR.Get())
				{
					ImGui::SliderFloat("Ray Step", SSRRayStep.GetPtr(), 1.0f, 3.0f);
					ImGui::SliderFloat("Ray Hit Threshold", SSRRayHitThreshold.GetPtr(), 0.25f, 5.0f);
				}
				ImGui::TreePop();
				ImGui::Separator();
			}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_Reflection);
	}

	void SSRPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_Ssr;
		ssr_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

}

