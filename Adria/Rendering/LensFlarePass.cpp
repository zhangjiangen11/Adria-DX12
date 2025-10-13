#include "LensFlarePass.h"
#include "ShaderStructs.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "PostProcessor.h" 
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "TextureManager.h"
#include "Core/Paths.h"
#include "Core/ConsoleManager.h"
#include "Editor/GUICommand.h"

using namespace DirectX;

namespace adria
{
	ADRIA_LOG_CHANNEL(PostProcessor);

	static TAutoConsoleVariable<Int> LensFlare("r.LensFlare.Type", 0, "0 - procedural, 1 - texture-based");
	enum LensFlareType : Uint8
	{
		LensFlareType_Procedural,
		LensFlareType_TextureBased,
	};

	LensFlarePass::LensFlarePass(GfxDevice* gfx, Uint32 w, Uint32 h)
		: gfx(gfx), width(w), height(h)
	{
		CreatePSOs();
		is_procedural_supported = gfx->GetCapabilities().SupportsTypedUAVLoadAdditionalFormats();
	}

	Bool LensFlarePass::IsEnabled(PostProcessor const* postprocessor) const
	{
		auto lights = postprocessor->GetRegistry().view<Light>();
		for (entt::entity light : lights)
		{
			Light const& light_data = lights.get<Light>(light);
			if (light_data.active && light_data.lens_flare)
			{
				return true;
			}
		}
		return false;
	}

	void LensFlarePass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		auto lights = postprocessor->GetRegistry().view<Light>();
		for (entt::entity light : lights)
		{
			auto const& light_data = lights.get<Light>(light);
			if (!light_data.active || !light_data.lens_flare)
			{
				continue;
			}

			switch (LensFlare.Get())
			{
			case LensFlareType_Procedural:
				AddProceduralLensFlarePass(rg, postprocessor, light_data);
				break;
			case LensFlareType_TextureBased:
			default:
				AddTextureBasedLensFlare(rg, postprocessor, light_data);
				break;
			}
		}
	}

	void LensFlarePass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	void LensFlarePass::OnSceneInitialized()
	{
		if (lens_flare_textures.empty())
		{
			lens_flare_textures.push_back(g_TextureManager.LoadTexture(paths::TexturesDir + "LensFlare/flare0.jpg"));
			lens_flare_textures.push_back(g_TextureManager.LoadTexture(paths::TexturesDir + "LensFlare/flare1.jpg"));
			lens_flare_textures.push_back(g_TextureManager.LoadTexture(paths::TexturesDir + "LensFlare/flare2.jpg"));
			lens_flare_textures.push_back(g_TextureManager.LoadTexture(paths::TexturesDir + "LensFlare/flare3.jpg"));
			lens_flare_textures.push_back(g_TextureManager.LoadTexture(paths::TexturesDir + "LensFlare/flare4.jpg"));
			lens_flare_textures.push_back(g_TextureManager.LoadTexture(paths::TexturesDir + "LensFlare/flare5.jpg"));
			lens_flare_textures.push_back(g_TextureManager.LoadTexture(paths::TexturesDir + "LensFlare/flare6.jpg"));
		}
	}

	void LensFlarePass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("Lens Flare", ImGuiTreeNodeFlags_None))
				{
					ImGui::Combo("Lens Flare Type", LensFlare.GetPtr(), "Procedural\0Texture-based\0", 2);
					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing
		);
	}

	Bool LensFlarePass::IsGUIVisible(PostProcessor const* postprocessor) const
	{
		return IsEnabled(postprocessor);
	}

	void LensFlarePass::CreatePSOs()
	{
		GfxGraphicsPipelineStateDesc gfx_pso_desc{};
		gfx_pso_desc.root_signature = GfxRootSignatureID::Common;
		gfx_pso_desc.VS = VS_LensFlare;
		gfx_pso_desc.GS = GS_LensFlare;
		gfx_pso_desc.PS = PS_LensFlare;
		gfx_pso_desc.blend_state.render_target[0].blend_enable = true;
		gfx_pso_desc.blend_state.render_target[0].src_blend = GfxBlend::One;
		gfx_pso_desc.blend_state.render_target[0].dest_blend = GfxBlend::One;
		gfx_pso_desc.blend_state.render_target[0].blend_op = GfxBlendOp::Add;
		gfx_pso_desc.topology_type = GfxPrimitiveTopologyType::Point;
		gfx_pso_desc.num_render_targets = 1;
		gfx_pso_desc.rtv_formats[0] = GfxFormat::R16G16B16A16_FLOAT;
		lens_flare_pso = gfx->CreateManagedGraphicsPipelineState(gfx_pso_desc);

		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_LensFlare2;
		procedural_lens_flare_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

	void LensFlarePass::AddTextureBasedLensFlare(RenderGraph& rg, PostProcessor* postprocessor, Light const& light)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct LensFlarePassData
		{
			RGTextureReadOnlyId depth;
		};

		rg.AddPass<LensFlarePassData>("Lens Flare Pass",
			[=](LensFlarePassData& data, RenderGraphBuilder& builder)
			{
				builder.WriteRenderTarget(postprocessor->GetFinalResource(), RGLoadStoreAccessOp::Preserve_Preserve);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](LensFlarePassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				if (light.type != LightType::Directional)
				{
					ADRIA_LOG(WARNING, "Using Lens Flare on a Non-Directional Light Source");
					return;
				}
				Vector3 light_ss{};
				{
					Vector4 camera_position(frame_data.camera_position);
					Vector4 light_pos = light.type == LightType::Directional ? Vector4::Transform(light.position, XMMatrixTranslation(camera_position.x, 0.0f, camera_position.y)) : light.position;
					light_pos = Vector4::Transform(light_pos, frame_data.camera_viewproj);

					light_ss.x = 0.5f * light_pos.x / light_pos.w + 0.5f;
					light_ss.y = -0.5f * light_pos.y / light_pos.w + 0.5f;
					light_ss.z = light_pos.z / light_pos.w;
				}

				GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU(1);
				gfx->CopyDescriptors(1, dst_descriptor, ctx.GetReadOnlyTexture(data.depth));
				Uint32 i = dst_descriptor.GetIndex();

				ADRIA_ASSERT(lens_flare_textures.size() == 7);
				struct LensFlareConstants
				{
					Uint32   lens_idx0;
					Uint32   lens_idx1;
					Uint32   lens_idx2;
					Uint32   lens_idx3;
					Uint32   lens_idx4;
					Uint32   lens_idx5;
					Uint32   lens_idx6;
					Uint32   depth_idx;
				} constants =
				{
					.lens_idx0 = (Uint32)lens_flare_textures[0], .lens_idx1 = (Uint32)lens_flare_textures[1],
					.lens_idx2 = (Uint32)lens_flare_textures[2], .lens_idx3 = (Uint32)lens_flare_textures[3],
					.lens_idx4 = (Uint32)lens_flare_textures[4], .lens_idx5 = (Uint32)lens_flare_textures[5],
					.lens_idx6 = (Uint32)lens_flare_textures[6], .depth_idx = i
				};

				struct LensFlareConstants2
				{
					Float light_ss_x;
					Float light_ss_y;
					Float light_ss_z;
				} constants2 =
				{
					.light_ss_x = light_ss.x,
					.light_ss_y = light_ss.y,
					.light_ss_z = light_ss.z
				};
				cmd_list->SetPipelineState(lens_flare_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->SetRootCBV(2, constants2);
				cmd_list->SetPrimitiveTopology(GfxPrimitiveTopology::PointList);
				cmd_list->Draw(7);

			}, RGPassType::Graphics, RGPassFlags::None);
	}

	void LensFlarePass::AddProceduralLensFlarePass(RenderGraph& rg, PostProcessor* postprocessor, Light const& light)
	{
		if (!is_procedural_supported)
		{
			return;
		}

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct LensFlarePassData
		{
			RGTextureReadWriteId output;
			RGTextureReadOnlyId depth;
		};

		rg.AddPass<LensFlarePassData>("Procedural Lens Flare Pass",
			[=](LensFlarePassData& data, RenderGraphBuilder& builder)
			{
				data.output = builder.WriteTexture(postprocessor->GetFinalResource());
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[=](LensFlarePassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				if (light.type != LightType::Directional)
				{
					ADRIA_LOG(WARNING, "Using Lens Flare on a Non-Directional Light Source");
					return;
				}
				XMFLOAT3 light_ss{};
				{
					Vector4 camera_position(frame_data.camera_position);
					Vector4 light_pos = light.type == LightType::Directional ? Vector4::Transform(light.position, XMMatrixTranslation(camera_position.x, 0.0f, camera_position.y)) : light.position;
					light_pos = Vector4::Transform(light_pos, frame_data.camera_viewproj);
					light_ss.x = 0.5f * light_pos.x / light_pos.w + 0.5f;
					light_ss.y = -0.5f * light_pos.y / light_pos.w + 0.5f;
					light_ss.z = light_pos.z / light_pos.w;
				}
				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth),
					ctx.GetReadWriteTexture(data.output)
				};
				GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_descriptors));
				gfx->CopyDescriptors(dst_descriptor, src_descriptors);
				Uint32 const i = dst_descriptor.GetIndex();

				struct LensFlareConstants
				{
					Float light_ss_x;
					Float light_ss_y;
					Uint32 depth_idx;
					Uint32 output_idx;
				} constants =
				{
					.light_ss_x = light_ss.x,
					.light_ss_y = light_ss.y,
					.depth_idx = i + 0,
					.output_idx = i + 1
				};

				cmd_list->SetPipelineState(procedural_lens_flare_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);

			}, RGPassType::Compute, RGPassFlags::None);
	}

}

