#include "HBAOPass.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "Utilities/Random.h"
#include "Editor/GUICommand.h"

namespace adria
{
	
	HBAOPass::HBAOPass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h), hbao_random_texture(nullptr), blur_pass(gfx)
	{
		CreatePSO();
	}
	HBAOPass::~HBAOPass() = default;

	void HBAOPass::AddPass(RenderGraph& rg)
	{
		RG_SCOPE(rg, "HBAO");

		struct HBAOPassData
		{
			RGTextureReadOnlyId gbuffer_normal_srv;
			RGTextureReadOnlyId depth_stencil_srv;
			RGTextureReadWriteId output_uav;
		};

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		rg.AddPass<HBAOPassData>("HBAO Pass",
			[=](HBAOPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc hbao_desc{};
				hbao_desc.format = GfxFormat::R8_UNORM;
				hbao_desc.width = width;
				hbao_desc.height = height;

				builder.DeclareTexture(RG_NAME(HBAO_Output), hbao_desc);
				data.output_uav = builder.WriteTexture(RG_NAME(HBAO_Output));
				data.gbuffer_normal_srv = builder.ReadTexture(RG_NAME(GBufferNormal), ReadAccess_NonPixelShader);
				data.depth_stencil_srv = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[&](HBAOPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth_stencil_srv),
					ctx.GetReadOnlyTexture(data.gbuffer_normal_srv),
					hbao_random_texture_srv,
					ctx.GetReadWriteTexture(data.output_uav)
				};
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_descriptors);

				struct HBAOConstants
				{
					Float  r2;
					Float  radius_to_screen;
					Float  power;
					Float  noise_scale;

					Uint32   depth_idx;
					Uint32   normal_idx;
					Uint32   noise_idx;
					Uint32   output_idx;
				} constants =
				{
					.r2 = params.hbao_radius * params.hbao_radius, .radius_to_screen = params.hbao_radius * 0.5f * Float(height) / (tanf(frame_data.camera_fov * 0.5f) * 2.0f),
					.power = params.hbao_power,
					.noise_scale = std::max(width * 1.0f / NOISE_DIM,height * 1.0f / NOISE_DIM),
					.depth_idx = table, .normal_idx = table + 1, .noise_idx = table + 2, .output_idx = table + 3
				};

				cmd_list->SetPipelineState(hbao_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
			}, RGPassType::AsyncCompute);

		blur_pass.AddPass(rg, RG_NAME(HBAO_Output), RG_NAME(AmbientOcclusion), " HBAO");
	}

	void HBAOPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("HBAO", ImGuiTreeNodeFlags_None))
				{
					ImGui::SliderFloat("Power", &params.hbao_power, 1.0f, 16.0f);
					ImGui::SliderFloat("Radius", &params.hbao_radius, 0.25f, 8.0f);

					ImGui::TreePop();
					ImGui::Separator();
				}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_AO);
	}

	void HBAOPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	void HBAOPass::OnSceneInitialized()
	{
		RealRandomGenerator rand_float{ 0.0f, 1.0f };
		std::vector<Float> random_texture_data;
		for (Int32 i = 0; i < 8 * 8; i++)
		{
			Float rand = rand_float();
			random_texture_data.push_back(sin(rand));
			random_texture_data.push_back(cos(rand));
			random_texture_data.push_back(rand_float());
			random_texture_data.push_back(rand_float());
		}

		GfxTextureSubData data{};
		data.data = random_texture_data.data();
		data.row_pitch = 8 * 4 * sizeof(Float);
		data.slice_pitch = 0;

		GfxTextureData init_data{};
		init_data.sub_data = &data;
		init_data.sub_count = 1;

		GfxTextureDesc noise_desc{};
		noise_desc.width = NOISE_DIM;
		noise_desc.height = NOISE_DIM;
		noise_desc.format = GfxFormat::R32G32B32A32_FLOAT;
		noise_desc.initial_state = GfxResourceState::PixelSRV;
		noise_desc.bind_flags = GfxBindFlag::ShaderResource;

		hbao_random_texture = gfx->CreateTexture(noise_desc, init_data);
		hbao_random_texture->SetName("HBAO Random Texture");
		hbao_random_texture_srv = gfx->CreateTextureSRV(hbao_random_texture.get());
	}

	void HBAOPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_Hbao;
		hbao_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

}

