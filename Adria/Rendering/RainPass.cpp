#include "RainPass.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "TextureManager.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxBufferView.h"
#include "Graphics/GfxPipelineState.h"
#include "Editor/GUICommand.h"
#include "Core/Paths.h"
#include "Core/ConsoleManager.h"
#include "Utilities/Random.h"

namespace adria
{
	static TAutoConsoleVariable<Bool> Rain("r.Rain", false, "Enable Rain");
	
	struct RainData
	{
		Vector3 position;
		Vector3 velocity;
		Float   state;
	};
	
	RainPass::RainPass(entt::registry& reg, GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h), rain_blocker_map_pass(reg, gfx, w, h)
	{
		CreatePSOs();
		Rain->AddOnChanged(ConsoleVariableDelegate::CreateLambda([this](IConsoleVariable* cvar) { OnRainEnabled(cvar->GetBool()); }));
	}

	void RainPass::Update(Float dt)
	{
		if (!pause_simulation)
		{
			rain_total_time += dt;
		}
	}

	void RainPass::AddBlockerPass(RenderGraph& rg)
	{
		rain_blocker_map_pass.AddPass(rg);
	}

	void RainPass::AddPass(RenderGraph& rg)
	{
		rg.ImportBuffer(RG_NAME(RainDataBuffer), rain_data_buffer.get());
		if (!pause_simulation)
		{
			AddSimulationPass(rg);
		}
		AddDrawPass(rg);
	}

	void RainPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("Rain Settings", 0))
				{
					if (ImGui::Checkbox("Enable", Rain.GetPtr()))
					{
						OnRainEnabled(Rain.Get());
					}
					if (Rain.Get())
					{
						ImGui::Checkbox("Pause simulation", &pause_simulation);
						if (ImGui::Checkbox("Cheap", &cheap))
						{
							OnRainEnabled(true);
						}
						ImGui::SliderFloat("Simulation speed", &simulation_speed, 0.1f, 10.0f);
						ImGui::SliderFloat("Rain density", &rain_density, 0.0f, 1.0f);
						ImGui::SliderFloat("Range radius", &range_radius, 10.0f, 50.0f);
						ImGui::SliderFloat("Streak scale", &streak_scale, 0.01f, 1.0f);
					}
					ImGui::TreePop();
					ImGui::Separator();
				}
			}, GUICommandGroup_Renderer);
	}

	Bool RainPass::IsEnabled() const
	{
		return Rain.Get();
	}

	void RainPass::OnSceneInitialized()
	{
		if (rain_data_buffer != nullptr)
		{
			return;
		}

		rain_streak_handle = g_TextureManager.LoadTexture(paths::TexturesDir + "Rain/RainStreak.dds");
		rain_splash_bump_handle = g_TextureManager.LoadTexture(paths::TexturesDir + "Rain/SplashBump.dds");
		rain_splash_diffuse_handle = g_TextureManager.LoadTexture(paths::TexturesDir + "Rain/SplashDiffuse.dds");

		GfxBufferDesc rain_data_buffer_desc = StructuredBufferDesc<RainData>(MAX_RAIN_DATA_BUFFER_SIZE);
		std::vector<RainData> rain_data_buffer_init(MAX_RAIN_DATA_BUFFER_SIZE);

		RealRandomGenerator<Float> rng(-1.0f, 1.0f);
		for (Uint64 i = 0; i < MAX_RAIN_DATA_BUFFER_SIZE; ++i)
		{
			rain_data_buffer_init[i].position = Vector3(0.0f, -1000.0f, 0.0f);
			rain_data_buffer_init[i].velocity = Vector3(0.0f, -10.0f + rng(), 0.0f);
			rain_data_buffer_init[i].state = 0.0f;
		}
		rain_data_buffer = gfx->CreateBuffer(rain_data_buffer_desc, rain_data_buffer_init.data());
	}

	void RainPass::CreatePSOs()
	{
		GfxGraphicsPipelineStateDesc gfx_pso_desc{};
		gfx_pso_desc.root_signature = GfxRootSignatureID::Common;
		gfx_pso_desc.VS = VS_Rain;
		gfx_pso_desc.PS = PS_Rain;
		gfx_pso_desc.num_render_targets = 1;
		gfx_pso_desc.rtv_formats[0] = GfxFormat::R16G16B16A16_FLOAT;
		gfx_pso_desc.depth_state.depth_enable = true;
		gfx_pso_desc.dsv_format = GfxFormat::D32_FLOAT;
		gfx_pso_desc.blend_state.render_target[0].blend_enable = true;
		gfx_pso_desc.blend_state.render_target[0].src_blend = GfxBlend::SrcAlpha;
		gfx_pso_desc.blend_state.render_target[0].dest_blend = GfxBlend::InvSrcAlpha;
		gfx_pso_desc.blend_state.render_target[0].blend_op = GfxBlendOp::Add;
		gfx_pso_desc.blend_state.render_target[0].blend_op_alpha = GfxBlendOp::Max;
		gfx_pso_desc.blend_state.render_target[0].src_blend_alpha = GfxBlend::One;
		gfx_pso_desc.blend_state.render_target[0].dest_blend_alpha = GfxBlend::One;
		gfx_pso_desc.rasterizer_state.cull_mode = GfxCullMode::None;
		rain_pso = gfx->CreateManagedGraphicsPipelineState(gfx_pso_desc);

		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_RainSimulation;
		rain_simulation_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

	void RainPass::AddSimulationPass(RenderGraph& rg)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct RainSimulationPassData
		{
			RGBufferReadWriteId rain_data_buffer;
		};
		rg.AddPass<RainSimulationPassData>("Rain Simulation Pass",
			[=](RainSimulationPassData& data, RenderGraphBuilder& builder)
			{
				data.rain_data_buffer = builder.WriteBuffer(RG_NAME(RainDataBuffer));
			},
			[=](RainSimulationPassData const& data, RenderGraphContext& context)
			{
				GfxDevice* gfx = context.GetDevice();
				GfxCommandList* cmd_list = context.GetCommandList();

				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(context.GetReadWriteBuffer(data.rain_data_buffer));
				struct RainSimulationConstants
				{
					Uint32   rain_data_idx;
					Uint32   depth_idx;
					Float    simulation_speed;
					Float    range_radius;
				} constants =
				{
					.rain_data_idx = table,
					.simulation_speed = simulation_speed,
					.range_radius = range_radius
				};

				cmd_list->SetPipelineState(rain_simulation_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(MAX_RAIN_DATA_BUFFER_SIZE, 256), 1, 1);

			}, RGPassType::Compute, RGPassFlags::None);
	}

	void RainPass::AddDrawPass(RenderGraph& rg)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct RainDrawPassData
		{
			RGBufferReadOnlyId rain_data_buffer;
		};

		rg.AddPass<RainDrawPassData>("Rain Draw Pass",
			[=](RainDrawPassData& data, RenderGraphBuilder& builder)
			{
				data.rain_data_buffer = builder.ReadBuffer(RG_NAME(RainDataBuffer), ReadAccess_NonPixelShader);
				builder.WriteRenderTarget(RG_NAME(HDR_RenderTarget), RGLoadStoreAccessOp::Preserve_Preserve);
				builder.WriteDepthStencil(RG_NAME(DepthStencil), RGLoadStoreAccessOp::Preserve_Preserve);
				builder.SetViewport(width, height);
			},
			[=](RainDrawPassData const& data, RenderGraphContext& context)
			{
				GfxDevice* gfx = context.GetDevice();
				GfxCommandList* cmd_list = context.GetCommandList();

				GfxDescriptor src_handle[] = { context.GetReadOnlyBuffer(data.rain_data_buffer) };
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_handle);

				struct Constants
				{
					Uint32   rain_data_idx;
					Uint32   rain_streak_idx;
					Float	 rain_streak_scale;

				} constants =
				{
					.rain_data_idx = table,
					.rain_streak_idx = (Uint32)rain_streak_handle,
					.rain_streak_scale = streak_scale
				};

				cmd_list->SetPipelineState(rain_pso->Get());
				cmd_list->SetPrimitiveTopology(GfxPrimitiveTopology::TriangleList);
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Draw(Uint32(rain_density * MAX_RAIN_DATA_BUFFER_SIZE) * 6);
			},
			RGPassType::Graphics, RGPassFlags::None);
	}
}
