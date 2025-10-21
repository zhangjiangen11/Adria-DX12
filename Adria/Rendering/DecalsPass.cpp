#include "DecalsPass.h"
#include "ShaderStructs.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h"
#include "TextureManager.h"
#include "Graphics/GfxBufferView.h"
#include "Graphics/GfxShaderCompiler.h"
#include "Graphics/GfxPipelineStatePermutations.h"
#include "RenderGraph/RenderGraph.h"
#include "entt/entity/registry.hpp"

using namespace DirectX;

namespace adria
{

	DecalsPass::DecalsPass(entt::registry& reg, GfxDevice* gfx, Uint32 w, Uint32 h)
	 : reg{ reg }, gfx{ gfx }, width{ w }, height{ h }
	{
		CreatePSOs();
	}
	DecalsPass::~DecalsPass() = default;

	void DecalsPass::AddPass(RenderGraph& rendergraph)
	{
		if (reg.view<Decal>().size() == 0)
		{
			return;
		}

		FrameBlackboardData const& frame_data = rendergraph.GetBlackboard().Get<FrameBlackboardData>();

		struct DecalsPassData
		{
			RGTextureReadOnlyId depth_srv;
		};
		rendergraph.AddPass<DecalsPassData>("Decals Pass",
			[=](DecalsPassData& data, RenderGraphBuilder& builder)
			{
				data.depth_srv = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_PixelShader);
				builder.WriteRenderTarget(RG_NAME(GBufferAlbedo), RGLoadStoreAccessOp::Preserve_Preserve);
				builder.WriteRenderTarget(RG_NAME(GBufferNormal), RGLoadStoreAccessOp::Preserve_Preserve);
				builder.SetViewport(width, height);
			},
			[=](DecalsPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth_srv)
				};
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_descriptors);

				struct DecalsConstants
				{
					Matrix model_matrix;
					Matrix transposed_inverse_model;
					Uint32 decal_type;
					Uint32 decal_albedo_idx;
					Uint32 decal_normal_idx;
					Uint32 depth_idx;
				} constants = 
				{
					.depth_idx = table.base
				};

				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				auto decal_view = reg.view<Decal>();

				auto decal_pass_lambda = [&](Bool modify_normals)
				{
					if (decal_view.empty()) return;

					if (modify_normals)
					{
						using enum GfxShaderStage;
						decal_psos->AddDefine<PS>("DECAL_MODIFY_NORMALS");
						decal_psos->ModifyDesc([](GfxGraphicsPipelineStateDesc& desc)
							{
								desc.num_render_targets = 2;
								desc.rtv_formats[1] = GfxFormat::R8G8B8A8_UNORM;
							});
					}
					GfxPipelineState const* pso = decal_psos->Get();
					cmd_list->SetPipelineState(pso);
					for (auto e : decal_view)
					{
						Decal& decal = decal_view.get<Decal>(e);
						if (decal.modify_gbuffer_normals != modify_normals) continue;

						constants.model_matrix = decal.decal_model_matrix;
						constants.transposed_inverse_model = decal.decal_model_matrix.Invert().Transpose(); 
						constants.decal_type = static_cast<Uint32>(decal.decal_type);
						constants.decal_albedo_idx = (Uint32)decal.albedo_decal_texture;
						constants.decal_normal_idx = (Uint32)decal.normal_decal_texture;
						
						cmd_list->SetRootCBV(2, constants);
						cmd_list->SetPrimitiveTopology(GfxPrimitiveTopology::TriangleList);
						cmd_list->SetVertexBuffer(GfxVertexBufferView(cube_vb.get()));
						GfxIndexBufferView ibv(cube_ib.get());
						cmd_list->SetIndexBuffer(&ibv);
						cmd_list->DrawIndexed(cube_ib->GetCount());
					}
				};
				decal_pass_lambda(false);
				decal_pass_lambda(true);
			}, RGPassType::Graphics, RGPassFlags::None);
	}

	void DecalsPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	void DecalsPass::OnSceneInitialized()
	{
		if (!cube_vb || !cube_ib)
		{
			CreateCubeBuffers();
		}
	}

	void DecalsPass::CreatePSOs()
	{
		GfxGraphicsPipelineStateDesc decals_pso_desc{};
		GfxShaderCompiler::FillInputLayoutDesc(SM_GetGfxShader(VS_Decals), decals_pso_desc.input_layout);
		decals_pso_desc.root_signature = GfxRootSignatureID::Common;
		decals_pso_desc.VS = VS_Decals;
		decals_pso_desc.PS = PS_Decals;
		decals_pso_desc.rasterizer_state.cull_mode = GfxCullMode::None;
		decals_pso_desc.depth_state.depth_enable = false;
		decals_pso_desc.num_render_targets = 1;
		decals_pso_desc.rtv_formats[0] = GfxFormat::R8G8B8A8_UNORM;
		decal_psos = std::make_unique<GfxGraphicsPipelineStatePermutations>(gfx, decals_pso_desc);
	}

	void DecalsPass::CreateCubeBuffers()
	{
		SimpleVertex const cube_vertices[8] =
		{
			Vector3{ -0.5f, -0.5f,  0.5f },
			Vector3{  0.5f, -0.5f,  0.5f },
			Vector3{  0.5f,  0.5f,  0.5f },
			Vector3{ -0.5f,  0.5f,  0.5f },
			Vector3{ -0.5f, -0.5f, -0.5f },
			Vector3{  0.5f, -0.5f, -0.5f },
			Vector3{  0.5f,  0.5f, -0.5f },
			Vector3{ -0.5f,  0.5f, -0.5f }
		};

		Uint16 const cube_indices[36] =
		{
			// front
			0, 1, 2,
			2, 3, 0,
			// right
			1, 5, 6,
			6, 2, 1,
			// back
			7, 6, 5,
			5, 4, 7,
			// left
			4, 0, 3,
			3, 7, 4,
			// bottom
			4, 5, 1,
			1, 0, 4,
			// top
			3, 2, 6,
			6, 7, 3
		};

		GfxBufferDesc vb_desc{};
		vb_desc.bind_flags = GfxBindFlag::None;
		vb_desc.size = sizeof(cube_vertices);
		vb_desc.stride = sizeof(SimpleVertex);
		cube_vb = gfx->CreateBuffer(vb_desc, cube_vertices);

		GfxBufferDesc ib_desc{};
		ib_desc.bind_flags = GfxBindFlag::None;
		ib_desc.format = GfxFormat::R16_UINT;
		ib_desc.stride = sizeof(Uint16);
		ib_desc.size = sizeof(cube_indices);
		cube_ib = gfx->CreateBuffer(ib_desc, cube_indices);
	}

}

