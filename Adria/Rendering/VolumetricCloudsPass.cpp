#include "VolumetricCloudsPass.h"
#include "ShaderStructs.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "Postprocessor.h" 
#include "TextureManager.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/GfxTexture.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineStatePermutations.h"
#include "Editor/GUICommand.h"
#include "Core/ConsoleManager.h"

using namespace DirectX;

namespace adria
{
	static TAutoConsoleVariable<Bool> Clouds("r.Clouds", true, "Enable or disable Clouds");
	static TAutoConsoleVariable<Bool> TemporalReprojection("r.Clouds.TemporalReprojection", true, "Should Clouds use temporal reprojection or not");
		
	VolumetricCloudsPass::VolumetricCloudsPass(GfxDevice* gfx, Uint32 w, Uint32 h)
		: gfx(gfx), width{ w }, height{ h }
	{
		CreatePSOs();
	}

	VolumetricCloudsPass::~VolumetricCloudsPass() = default;

	Bool VolumetricCloudsPass::IsEnabled(PostProcessor const*) const
	{
		return Clouds.Get();
	}

	void VolumetricCloudsPass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		RG_SCOPE(rg, "Volumetric Clouds");
		rg.ImportTexture(RG_NAME(PreviousCloudsOutput), prev_clouds.get());
		AddComputeTexturesPass(rg);
		AddComputeCloudsPass(rg);
		if (TemporalReprojection.Get())
		{
			AddCopyPassForTemporalReprojection(rg);
		}
		AddCombinePass(rg, postprocessor->GetFinalResource());
	}

	void VolumetricCloudsPass::AddCombinePass(RenderGraph& rendergraph, RGResourceName render_target)
	{
		struct CloudsCombinePassData
		{
			RGTextureReadOnlyId clouds_src;
		};

		rendergraph.AddPass<CloudsCombinePassData>("Volumetric Clouds Combine Pass",
			[=](CloudsCombinePassData& data, RenderGraphBuilder& builder)
			{
				builder.WriteRenderTarget(render_target, RGLoadStoreAccessOp::Preserve_Preserve);
				builder.WriteDepthStencil(RG_NAME(DepthStencil), RGLoadStoreAccessOp::Preserve_Preserve);
				data.clouds_src = builder.ReadTexture(RG_NAME(CloudsOutput), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](CloudsCombinePassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				cmd_list->SetPipelineState(clouds_combine_pso->Get());
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(ctx.GetReadOnlyTexture(data.clouds_src));
				cmd_list->SetRootConstant(1, table, 0);
				cmd_list->SetPrimitiveTopology(GfxPrimitiveTopology::TriangleList);
				cmd_list->Draw(3);
			}, RGPassType::Graphics, RGPassFlags::None);
	}

	void VolumetricCloudsPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
		if (prev_clouds)
		{
			if (!gfx) gfx = cloud_shape_noise->GetParent();

			GfxTextureDesc clouds_output_desc = prev_clouds->GetDesc();
			clouds_output_desc.width = width >> resolution;
			clouds_output_desc.height = height >> resolution;
			prev_clouds = gfx->CreateTexture(clouds_output_desc);
		}
	}

	void VolumetricCloudsPass::OnSceneInitialized()
	{
		GfxTextureDesc clouds_output_desc{};
		clouds_output_desc.clear_value = GfxClearValue(0.0f, 0.0f, 0.0f, 0.0f);
		clouds_output_desc.width = width >> resolution;
		clouds_output_desc.height = height >> resolution;
		clouds_output_desc.format = GfxFormat::R16G16B16A16_FLOAT;
		clouds_output_desc.bind_flags = GfxBindFlag::ShaderResource;
		clouds_output_desc.initial_state = GfxResourceState::ComputeSRV;

		prev_clouds = gfx->CreateTexture(clouds_output_desc);
		CreateCloudTextures(gfx);
	}

	void VolumetricCloudsPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("Volumetric Clouds", 0))
				{
					ImGui::Checkbox("Enable Volumetric Clouds", Clouds.GetPtr());
					if (Clouds.Get())
					{
						ImGui::Checkbox("Temporal Reprojection", TemporalReprojection.GetPtr());
						should_generate_textures |= ImGui::SliderInt("Shape Noise Frequency", &params.shape_noise_frequency, 1, 10);
						should_generate_textures |= ImGui::SliderInt("Shape Noise Resolution", &params.shape_noise_resolution, 32, 256);
						should_generate_textures |= ImGui::SliderInt("Detail Noise Frequency", &params.detail_noise_frequency, 1, 10);
						should_generate_textures |= ImGui::SliderInt("Detail Noise Resolution", &params.detail_noise_resolution, 8, 64);

						ImGui::SliderFloat("Cloud Type", &params.cloud_type, 0.0f, 1.0f);
						ImGui::SliderFloat("Global Density", &params.global_density, 0.0f, 1.0f);
						ImGui::SliderFloat("Cloud Min Height", &params.cloud_min_height, 1000.0f, 20000.0f);
						ImGui::SliderFloat("Cloud Max Height", &params.cloud_max_height, params.cloud_min_height, 20000.0f);
						ImGui::SliderFloat("Shape Noise Scale", &params.shape_noise_scale, 0.1f, 1.0f);
						ImGui::SliderFloat("Detail Noise Scale", &params.detail_noise_scale, 0.0f, 100.0f);
						ImGui::SliderFloat("Detail Noise Modifier", &params.detail_noise_modifier, 0.0f, 1.0f);
						ImGui::SliderFloat("Cloud Coverage", &params.cloud_coverage, 0.0f, 1.0f);
						ImGui::SliderFloat("Precipitation", &params.precipitation, 1.0f, 2.5f);
						ImGui::SliderFloat("Ambient Factor", &params.ambient_light_factor, 0.01f, 10.0f);
						ImGui::SliderFloat("Sun Factor", &params.sun_light_factor, 0.1f, 10.0f);
						ImGui::InputFloat("Planet Radius", &params.planet_radius);
						ImGui::SliderInt("Max Num Steps", &params.max_num_steps, 16, 256);

						static Int _resolution = (Int)resolution;
						if (ImGui::Combo("Volumetric Clouds Resolution", &_resolution, "Full\0Half\0Quarter\0", 3))
						{
							resolution = (CloudResolution)_resolution;
							OnResize(width, height);
						}

						if (resolution != _resolution)
						{
							resolution = (CloudResolution)_resolution;
							OnResize(width, height);
						}
					}
					ImGui::TreePop();
					ImGui::Separator();
				}
			}, GUICommandGroup_PostProcessing
		);
	}

	void VolumetricCloudsPass::OnRainEvent(Bool enabled)
	{
		if (enabled)
		{
			params.precipitation = 2.5f;
			params.cloud_coverage = 0.75f;
			params.sun_light_factor = 0.15f;
		}
		else
		{
			params.precipitation = 1.78f;
			params.cloud_coverage = 0.625f;
			params.sun_light_factor = 0.7f;
		}
	}

	void VolumetricCloudsPass::CreatePSOs()
	{
		GfxComputePipelineStateDesc clouds_pso_desc{};
		clouds_pso_desc.CS = CS_Clouds;
		clouds_psos = std::make_unique<GfxComputePipelineStatePermutations>(gfx, clouds_pso_desc);

		clouds_pso_desc.CS = CS_CloudType;
		clouds_type_pso = gfx->CreateManagedComputePipelineState(clouds_pso_desc);

		clouds_pso_desc.CS = CS_CloudShape;
		clouds_shape_pso = gfx->CreateManagedComputePipelineState(clouds_pso_desc);

		clouds_pso_desc.CS = CS_CloudDetail;
		clouds_detail_pso = gfx->CreateManagedComputePipelineState(clouds_pso_desc);


		GfxGraphicsPipelineStateDesc gfx_pso_desc{};
		gfx_pso_desc.root_signature = GfxRootSignatureID::Common;
		gfx_pso_desc.VS = VS_CloudsCombine;
		gfx_pso_desc.PS = PS_CloudsCombine;
		gfx_pso_desc.num_render_targets = 1;
		gfx_pso_desc.rtv_formats[0] = GfxFormat::R16G16B16A16_FLOAT;
		gfx_pso_desc.depth_state.depth_enable = true;
		gfx_pso_desc.dsv_format = GfxFormat::D32_FLOAT;
		gfx_pso_desc.blend_state.render_target[0].blend_enable = true;
		gfx_pso_desc.blend_state.render_target[0].src_blend = GfxBlend::One;
		gfx_pso_desc.blend_state.render_target[0].dest_blend = GfxBlend::InvSrcAlpha;
		gfx_pso_desc.blend_state.render_target[0].blend_op = GfxBlendOp::Add;
		gfx_pso_desc.rasterizer_state.cull_mode = GfxCullMode::None;
		clouds_combine_pso = gfx->CreateManagedGraphicsPipelineState(gfx_pso_desc);
	}

	void VolumetricCloudsPass::CreateCloudTextures(GfxDevice* gfx)
	{
		if (!gfx) gfx = cloud_shape_noise->GetParent();

		GfxTextureDesc cloud_shape_noise_desc{};
		cloud_shape_noise_desc.type = GfxTextureType_3D;
		cloud_shape_noise_desc.width = params.shape_noise_resolution;
		cloud_shape_noise_desc.height = params.shape_noise_resolution;
		cloud_shape_noise_desc.depth = params.shape_noise_resolution;
		cloud_shape_noise_desc.mip_levels = 4;
		cloud_shape_noise_desc.format = GfxFormat::R8G8B8A8_UNORM;
		cloud_shape_noise_desc.bind_flags = GfxBindFlag::ShaderResource | GfxBindFlag::UnorderedAccess;

		cloud_shape_noise = gfx->CreateTexture(cloud_shape_noise_desc);

		GfxTextureDesc cloud_detail_noise_desc{};
		cloud_detail_noise_desc.type = GfxTextureType_3D;
		cloud_detail_noise_desc.width = params.shape_noise_resolution;
		cloud_detail_noise_desc.height = params.shape_noise_resolution;
		cloud_detail_noise_desc.depth = params.shape_noise_resolution;
		cloud_detail_noise_desc.mip_levels = 4;
		cloud_detail_noise_desc.format = GfxFormat::R8G8B8A8_UNORM;
		cloud_detail_noise_desc.bind_flags = GfxBindFlag::ShaderResource | GfxBindFlag::UnorderedAccess;

		cloud_detail_noise = gfx->CreateTexture(cloud_detail_noise_desc);

		GfxTextureDesc cloud_type_desc{};
		cloud_type_desc.type = GfxTextureType_2D;
		cloud_type_desc.width = 128;
		cloud_type_desc.height = 128;
		cloud_type_desc.mip_levels = 1;
		cloud_type_desc.format = GfxFormat::R8_UNORM;
		cloud_type_desc.bind_flags = GfxBindFlag::ShaderResource | GfxBindFlag::UnorderedAccess;

		cloud_type = gfx->CreateTexture(cloud_type_desc);
	}

	void VolumetricCloudsPass::AddComputeTexturesPass(RenderGraph& rg)
	{
		static Bool first_frame = true;
		if (first_frame || should_generate_textures)
		{
			first_frame = false;
			if (should_generate_textures)
			{
				CreateCloudTextures();
				should_generate_textures = false;
			}
			rg.ImportTexture(RG_NAME(CloudShape), cloud_shape_noise.get());
			rg.ImportTexture(RG_NAME(CloudDetail), cloud_detail_noise.get());
			rg.ImportTexture(RG_NAME(CloudType), cloud_type.get());

			struct CloudNoiseConstants
			{
				Float resolution_inv;
				Uint32 frequency;
				Uint32 output_idx;
			};

			for (Uint32 i = 0; i < cloud_shape_noise->GetDesc().mip_levels; ++i)
			{
				struct CloudShapePassData
				{
					RGTextureReadWriteId shape;
				};

				rg.AddPass<CloudShapePassData>("Compute Cloud Shape",
					[=](CloudShapePassData& data, RenderGraphBuilder& builder)
					{
						data.shape = builder.WriteTexture(RG_NAME(CloudShape), i, 1);
					},
					[=](CloudShapePassData const& data, RenderGraphContext& ctx)
					{
						GfxDevice* gfx = ctx.GetDevice();
						GfxCommandList* cmd_list = ctx.GetCommandList();

						Uint32 const resolution = cloud_shape_noise->GetDesc().width >> i;
						GfxDescriptor src_handles[] = { ctx.GetReadWriteTexture(data.shape) };
						GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_handles);
						CloudNoiseConstants constants
						{
							.resolution_inv = 1.0f / resolution,
							.frequency = (Uint32)params.shape_noise_frequency,
							.output_idx = table
						};
						cmd_list->SetPipelineState(clouds_shape_pso->Get());
						cmd_list->SetRootConstants(1, constants);
						Uint32 const dispatch = DivideAndRoundUp(resolution, 8);
						cmd_list->Dispatch(dispatch, dispatch, dispatch);
					}, RGPassType::Compute, RGPassFlags::None);
			}

			for (Uint32 i = 0; i < cloud_detail_noise->GetDesc().mip_levels; ++i)
			{
				struct CloudShapePassData
				{
					RGTextureReadWriteId detail;
				};

				rg.AddPass<CloudShapePassData>("Compute Cloud Detail",
					[=](CloudShapePassData& data, RenderGraphBuilder& builder)
					{
						data.detail = builder.WriteTexture(RG_NAME(CloudDetail), i, 1);
					},
					[=](CloudShapePassData const& data, RenderGraphContext& ctx)
					{
						GfxDevice* gfx = ctx.GetDevice();
						GfxCommandList* cmd_list = ctx.GetCommandList();

						Uint32 const resolution = cloud_detail_noise->GetDesc().width >> i;
						GfxDescriptor src_handles[] = { ctx.GetReadWriteTexture(data.detail) };
						GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_handles);
						CloudNoiseConstants constants
						{
							.resolution_inv = 1.0f / resolution,
							.frequency = (Uint32)params.detail_noise_frequency,
							.output_idx = table
						};
						cmd_list->SetPipelineState(clouds_detail_pso->Get());
						cmd_list->SetRootConstants(1, constants);
						Uint32 const dispatch = DivideAndRoundUp(resolution, 8);
						cmd_list->Dispatch(dispatch, dispatch, dispatch);
					}, RGPassType::Compute, RGPassFlags::None);
			}

			struct CloudTypePassData
			{
				RGTextureReadWriteId type;
			};

			rg.AddPass<CloudTypePassData>("Compute Cloud Type",
				[=](CloudTypePassData& data, RenderGraphBuilder& builder)
				{
					data.type = builder.WriteTexture(RG_NAME(CloudType));
				},
				[=](CloudTypePassData const& data, RenderGraphContext& ctx)
				{
					GfxDevice* gfx = ctx.GetDevice();
					GfxCommandList* cmd_list = ctx.GetCommandList();

					Uint32 const resolution = cloud_type->GetDesc().width;
					GfxDescriptor src_handles[] = { ctx.GetReadWriteTexture(data.type) };
					GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_handles);
					CloudNoiseConstants constants
					{
						.resolution_inv = 1.0f / resolution,
						.output_idx = table
					};
					cmd_list->SetPipelineState(clouds_type_pso->Get());
					cmd_list->SetRootConstants(1, constants);
					Uint32 const dispatch = DivideAndRoundUp(resolution, 8);
					cmd_list->Dispatch(dispatch, dispatch, dispatch);
				}, RGPassType::Compute, RGPassFlags::None);
		}
		else
		{
			rg.ImportTexture(RG_NAME(CloudShape), cloud_shape_noise.get());
			rg.ImportTexture(RG_NAME(CloudDetail), cloud_detail_noise.get());
			rg.ImportTexture(RG_NAME(CloudType), cloud_type.get());
		}
	}

	void VolumetricCloudsPass::AddComputeCloudsPass(RenderGraph& rg)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct VolumetricCloudsPassData
		{
			RGTextureReadOnlyId type;
			RGTextureReadOnlyId shape;
			RGTextureReadOnlyId detail;
			RGTextureReadOnlyId prev_output;
			RGTextureReadOnlyId depth;
			RGTextureReadWriteId output;
		};
		rg.AddPass<VolumetricCloudsPassData>("Volumetric Clouds Compute Pass",
			[=](VolumetricCloudsPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc clouds_output_desc{};
				clouds_output_desc.clear_value = GfxClearValue(0.0f, 0.0f, 0.0f, 0.0f);
				clouds_output_desc.width = width >> resolution;
				clouds_output_desc.height = height >> resolution;
				clouds_output_desc.format = GfxFormat::R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_NAME(CloudsOutput), clouds_output_desc);

				data.output = builder.WriteTexture(RG_NAME(CloudsOutput));
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil));
				data.type = builder.ReadTexture(RG_NAME(CloudType), ReadAccess_NonPixelShader);
				data.shape = builder.ReadTexture(RG_NAME(CloudShape), ReadAccess_NonPixelShader);
				data.detail = builder.ReadTexture(RG_NAME(CloudDetail), ReadAccess_NonPixelShader);
				data.prev_output = builder.ReadTexture(RG_NAME(PreviousCloudsOutput), ReadAccess_NonPixelShader);
			},
			[=](VolumetricCloudsPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_handles[] = { ctx.GetReadOnlyTexture(data.type),
												ctx.GetReadOnlyTexture(data.shape),
												ctx.GetReadOnlyTexture(data.detail),
												ctx.GetReadOnlyTexture(data.depth),
												ctx.GetReadOnlyTexture(data.prev_output),
												ctx.GetReadWriteTexture(data.output) };
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_handles);

				Float noise_scale = 0.00001f + params.shape_noise_scale * 0.0004f;
				struct CloudsConstants
				{
					Uint32      type_idx;
					Uint32      shape_idx;
					Uint32      detail_idx;
					Uint32		depth_output_idx;

					Uint32      prev_output_idx;
					Uint32      output_idx;
					Float		cloud_type;
					Float 	    cloud_min_height;

					Float 	    cloud_max_height;
					Float 	    shape_noise_scale;
					Float 	    detail_noise_scale;
					Float 	    detail_noise_modifier;

					Float 	    cloud_coverage;
					Vector3     cloud_base_color;
					Vector3     cloud_top_color;
					Float       global_density;

					Int	        max_num_steps;
					Vector3     planet_center;

					Float 	    planet_radius;
					Float 	    light_step_length;
					Float 	    light_cone_radius;
					Float 	    precipitation;

					Float 	    ambient_light_factor;
					Float 	    sun_light_factor;
					Float 	    henyey_greenstein_g_forward;
					Float 	    henyey_greenstein_g_backward;
					Uint32      resolution_factor;
				} constants =
				{
					.type_idx = table + 0,
					.shape_idx = table + 1,
					.detail_idx = table + 2,
					.depth_output_idx = table + 3,

					.prev_output_idx = table + 4,
					.output_idx = table + 5,
					.cloud_type = params.cloud_type,
					.cloud_min_height = params.cloud_min_height,
					.cloud_max_height = params.cloud_max_height,

					.shape_noise_scale = noise_scale,
					.detail_noise_scale = params.detail_noise_scale * noise_scale,
					.detail_noise_modifier = params.detail_noise_modifier,

					.cloud_coverage = params.cloud_coverage,
					.cloud_base_color = Vector3(params.cloud_base_color),
					.cloud_top_color = Vector3(params.cloud_top_color),
					.global_density = params.global_density,

					.max_num_steps = params.max_num_steps,
					.planet_center = Vector3(0.0f, -params.planet_radius, 0.0f),

					.planet_radius = params.planet_radius,
					.light_step_length = params.light_step_length,
					.light_cone_radius = params.light_cone_radius,
					.precipitation = params.precipitation * 0.01f,

					.ambient_light_factor = params.ambient_light_factor,
					.sun_light_factor = params.sun_light_factor,
					.henyey_greenstein_g_forward = params.henyey_greenstein_g_forward,
					.henyey_greenstein_g_backward = params.henyey_greenstein_g_backward,
					.resolution_factor = (Uint32)resolution,
				};
				clouds_psos->AddDefine("REPROJECTION", TemporalReprojection.Get() ? "1" : "0");

				GfxPipelineState const* clouds_pso = clouds_psos->Get();
				cmd_list->SetPipelineState(clouds_pso);
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootCBV(2, constants);
				cmd_list->Dispatch(DivideAndRoundUp((width >> resolution), 16), DivideAndRoundUp((height >> resolution), 16), 1);

			}, RGPassType::Compute, RGPassFlags::None);
	}

	void VolumetricCloudsPass::AddCopyPassForTemporalReprojection(RenderGraph& rg)
	{
		struct CopyCloudsPassData
		{
			RGTextureCopySrcId copy_src;
			RGTextureCopyDstId copy_dst;
		};
		rg.AddPass<CopyCloudsPassData>("Volumetric Clouds Copy Pass",
			[=](CopyCloudsPassData& data, RenderGraphBuilder& builder)
			{
				data.copy_dst = builder.WriteCopyDstTexture(RG_NAME(PreviousCloudsOutput));
				data.copy_src = builder.ReadCopySrcTexture(RG_NAME(CloudsOutput));
			},
			[=](CopyCloudsPassData const& data, RenderGraphContext& ctx)
			{
				GfxCommandList* cmd_list = ctx.GetCommandList();
				GfxTexture const& src_texture = ctx.GetCopySrcTexture(data.copy_src);
				GfxTexture& dst_texture = ctx.GetCopyDstTexture(data.copy_dst);
				cmd_list->CopyTexture(dst_texture, src_texture);
			}, RGPassType::Copy, RGPassFlags::ForceNoCull);
	}

}
