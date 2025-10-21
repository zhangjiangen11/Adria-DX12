#include "ExponentialHeightFogPass.h"
#include "ShaderStructs.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "PostProcessor.h" 
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "TextureManager.h"
#include "Editor/GUICommand.h"
#include "Math/Packing.h"
#include "Core/ConsoleManager.h"

namespace adria
{

	static TAutoConsoleVariable<Bool> Fog("r.Fog", false, "Enable or Disable Fog");
	ExponentialHeightFogPass::ExponentialHeightFogPass(GfxDevice* gfx, Uint32 w, Uint32 h)
		: gfx(gfx), width(w), height(h), params()
	{
		CreatePSO();
	}

	void ExponentialHeightFogPass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		RGResourceName last_resource = postprocessor->GetFinalResource();

		struct ExponentialHeightFogPassData
		{
			RGTextureReadOnlyId depth;
			RGTextureReadOnlyId input;
			RGTextureReadWriteId output;
		};

		rg.AddPass<ExponentialHeightFogPassData>("Exponential Height Fog Pass",
			[=](ExponentialHeightFogPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc fog_output_desc{};
				fog_output_desc.width = width;
				fog_output_desc.height = height;
				fog_output_desc.format = GfxFormat::R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_NAME(FogOutput), fog_output_desc);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
				data.input = builder.ReadTexture(last_resource, ReadAccess_NonPixelShader);
				data.output = builder.WriteTexture(RG_NAME(FogOutput));
				builder.SetViewport(width, height);
			},
			[=](ExponentialHeightFogPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth),
					ctx.GetReadOnlyTexture(data.input),
					ctx.GetReadWriteTexture(data.output)
				};
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_descriptors);

				Float density = params.fog_density / 1000.0f;
				Float falloff = params.fog_falloff / 1000.0f;

				struct ExponentialHeightFogConstants
				{
					Float   fog_falloff;
					Float   fog_density;
					Float   fog_height;
					Float   fog_start;

					Float   fog_at_view_position;
					Float   fog_min_opacity;
					Float   fog_cutoff_distance;
					Uint32  fog_color;

					Uint32  depth_idx;
					Uint32  scene_idx;
					Uint32  output_idx;
				} constants =
				{
					.fog_falloff = params.fog_falloff, .fog_density = params.fog_density, .fog_height = params.fog_height, .fog_start = params.fog_start,
					.fog_at_view_position = density * pow(2.0f, -falloff * (frame_data.camera_position[1] - height)),
					.fog_min_opacity = params.fog_min_opacity,
					.fog_cutoff_distance = params.fog_cutoff_distance,
					.fog_color = PackToUint(params.fog_color),
					.depth_idx = table, .scene_idx = table + 1, .output_idx = table + 2
				};

				cmd_list->SetPipelineState(fog_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootCBV(2, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
			}, RGPassType::Compute, RGPassFlags::None);

		postprocessor->SetFinalResource(RG_NAME(FogOutput));
	}

	Bool ExponentialHeightFogPass::IsEnabled(PostProcessor const*) const
	{
		return Fog.Get();
	}

	void ExponentialHeightFogPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("Exponential Height Fog", 0))
				{
					ImGui::Checkbox("Enable Fog", Fog.GetPtr());
					if (Fog.Get())
					{
						ImGui::SliderFloat("Fog Falloff", &params.fog_falloff, 0.0001f, 10.0f);
						ImGui::SliderFloat("Fog Density", &params.fog_density, 0.0000001f, 100.0f);
						ImGui::SliderFloat("Fog Start", &params.fog_start, 0.0f, 10000.0f);
						ImGui::SliderFloat("Fog Min Opacity", &params.fog_min_opacity, 0.0f, 1.0f);
						ImGui::SliderFloat("Fog Cutoff Distance", &params.fog_cutoff_distance, 0.0f, 10000.0f);
						ImGui::SliderFloat("Fog Height", &params.fog_height, -10000.0f, 10000.0f);
						ImGui::ColorEdit3("Fog Color", params.fog_color);
					}
					ImGui::TreePop();
					ImGui::Separator();
				}
			}, GUICommandGroup_PostProcessing);
	}

	void ExponentialHeightFogPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	void ExponentialHeightFogPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_ExponentialHeightFog;
		fog_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

}


