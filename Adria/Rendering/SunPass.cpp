#include "SunPass.h"
#include "ShaderManager.h"
#include "Components.h"
#include "BlackboardData.h"
#include "PostProcessor.h"
#include "Graphics/GfxShaderCompiler.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "entt/entity/registry.hpp"


namespace adria
{

	SunPass::SunPass(GfxDevice* gfx, Uint32 width, Uint32 height) : gfx(gfx), width(width), height(height)
	{
		CreatePSO();
	}

	void SunPass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		entt::registry& reg = postprocessor->GetRegistry();
		entt::entity sun = entt::null;
		auto lights = reg.view<Light>();
		for (entt::entity light : lights)
		{
			Light const& light_data = lights.get<Light>(light);
			if (!light_data.active) continue;
			if (light_data.type == LightType::Directional)
			{
				sun = light;
				break;
			}
		}
		ADRIA_ASSERT(sun != entt::null);

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		RGResourceName last_resource = postprocessor->GetFinalResource();

		rg.AddPass<void>("Sun Pass",
			[=](RenderGraphBuilder& builder)
			{
				RGTextureDesc sun_output_desc{};
				sun_output_desc.format = GfxFormat::R16G16B16A16_FLOAT;
				sun_output_desc.width = width;
				sun_output_desc.height = height;
				sun_output_desc.clear_value = GfxClearValue(0.0f, 0.0f, 0.0f, 0.0f);

				builder.DeclareTexture(RG_NAME(SunOutput), sun_output_desc);
				builder.ReadDepthStencil(RG_NAME(DepthStencil), RGLoadStoreAccessOp::Preserve_Preserve);
				builder.WriteRenderTarget(RG_NAME(SunOutput), RGLoadStoreAccessOp::Clear_Preserve);
				builder.SetViewport(width, height);
			},
			[=, &reg](RenderGraphContext& context)
			{
				GfxDevice* gfx = context.GetDevice();
				GfxCommandList* cmd_list = context.GetCommandList();

				cmd_list->SetPipelineState(sun_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				auto [transform, mesh, material] = reg.get<Transform, SubMesh, Material>(sun);

				struct Constants
				{
					Matrix model_matrix;
					Vector3 diffuse_color;
					Uint32   diffuse_idx;
				} constants =
				{
					.model_matrix = transform.current_transform,
					.diffuse_color = Vector3(material.albedo_color),
					.diffuse_idx = (Uint32)material.albedo_texture
				};
				cmd_list->SetRootCBV(2, constants);
				Draw(mesh, cmd_list);
			}, RGPassType::Graphics, RGPassFlags::None);
	}

	void SunPass::CreatePSO()
	{
		GfxGraphicsPipelineStateDesc gfx_pso_desc{};
		GfxShaderCompiler::FillInputLayoutDesc(SM_GetGfxShader(VS_Sun), gfx_pso_desc.input_layout);
		gfx_pso_desc.root_signature = GfxRootSignatureID::Common;
		gfx_pso_desc.VS = VS_Sun;
		gfx_pso_desc.PS = PS_Texture;
		gfx_pso_desc.blend_state.render_target[0].blend_enable = true;
		gfx_pso_desc.blend_state.render_target[0].src_blend = GfxBlend::SrcAlpha;
		gfx_pso_desc.blend_state.render_target[0].dest_blend = GfxBlend::InvSrcAlpha;
		gfx_pso_desc.blend_state.render_target[0].blend_op = GfxBlendOp::Add;
		gfx_pso_desc.depth_state.depth_enable = true;
		gfx_pso_desc.depth_state.depth_write_mask = GfxDepthWriteMask::Zero;
		gfx_pso_desc.depth_state.depth_func = GfxComparisonFunc::GreaterEqual;
		gfx_pso_desc.num_render_targets = 1;
		gfx_pso_desc.rtv_formats[0] = GfxFormat::R16G16B16A16_FLOAT;
		gfx_pso_desc.dsv_format = GfxFormat::D32_FLOAT;
		sun_pso = gfx->CreateManagedGraphicsPipelineState(gfx_pso_desc);
	}

}
