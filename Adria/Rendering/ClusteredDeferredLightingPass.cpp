#include "ClusteredDeferredLightingPass.h"
#include "ShaderStructs.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "Graphics/GfxCommon.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "Math/Packing.h"
#include "entt/entity/registry.hpp"

using namespace DirectX;

namespace adria
{
	struct ClusterAABB
	{
		Vector4 min_point;
		Vector4 max_point;
	};

	struct LightGrid
	{
		Uint32 offset;
		Uint32 light_count;
	};

	ClusteredDeferredLightingPass::ClusteredDeferredLightingPass(entt::registry& reg, GfxDevice* gfx, Uint32 w, Uint32 h) 
		: reg(reg), gfx(gfx), width(w), height(h),
		clusters(gfx, StructuredBufferDesc<ClusterAABB>(CLUSTER_COUNT)),
		light_counter(gfx, StructuredBufferDesc<Uint32>(1)),
		light_list(gfx, StructuredBufferDesc<Uint32>(CLUSTER_COUNT * CLUSTER_MAX_LIGHTS)),
		light_grid(gfx, StructuredBufferDesc<LightGrid>(CLUSTER_COUNT))
	{
		CreatePSOs();
	}

	void ClusteredDeferredLightingPass::AddPass(RenderGraph& rendergraph, Bool recreate_clusters)
	{
		FrameBlackboardData const& frame_data = rendergraph.GetBlackboard().Get<FrameBlackboardData>();

		rendergraph.ImportBuffer(RG_NAME(ClustersBuffer), &clusters);
		rendergraph.ImportBuffer(RG_NAME(LightCounter), &light_counter);
		rendergraph.ImportBuffer(RG_NAME(LightGrid), &light_grid);
		rendergraph.ImportBuffer(RG_NAME(LightList), &light_list);

		struct ClusterBuildingPassData
		{
			RGBufferReadWriteId clusters;
		};

		if (recreate_clusters)
		{
			rendergraph.AddPass<ClusterBuildingPassData>("Cluster Building Pass",
				[=](ClusterBuildingPassData& data, RenderGraphBuilder& builder)
				{
					data.clusters = builder.WriteBuffer(RG_NAME(ClustersBuffer));
				},
				[=](ClusterBuildingPassData const& data, RenderGraphContext& context, GfxCommandList* cmd_list)
				{
					GfxDevice* gfx = cmd_list->GetDevice();
					GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU();
					gfx->CopyDescriptors(1, dst_descriptor, context.GetReadWriteBuffer(data.clusters));

					cmd_list->SetPipelineState(clustered_building_pso.get());
					cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
					cmd_list->SetRootConstant(1, dst_descriptor.GetIndex(), 0);
					cmd_list->Dispatch(CLUSTER_SIZE_X, CLUSTER_SIZE_Y, CLUSTER_SIZE_Z);
				}, RGPassType::Compute, RGPassFlags::None);
		}

		struct ClusterCullingPassData
		{
			RGBufferReadOnlyId  clusters;
			RGBufferReadWriteId light_counter;
			RGBufferReadWriteId light_grid;
			RGBufferReadWriteId light_list;
		};
		rendergraph.AddPass<ClusterCullingPassData>("Cluster Culling Pass",
			[=](ClusterCullingPassData& data, RenderGraphBuilder& builder)
			{
				data.clusters = builder.ReadBuffer(RG_NAME(ClustersBuffer), ReadAccess_NonPixelShader);
				data.light_counter = builder.WriteBuffer(RG_NAME(LightCounter));
				data.light_grid = builder.WriteBuffer(RG_NAME(LightGrid));
				data.light_list = builder.WriteBuffer(RG_NAME(LightList));
			},
			[=](ClusterCullingPassData const& data, RenderGraphContext& context, GfxCommandList* cmd_list)
			{
				GfxDevice* gfx = cmd_list->GetDevice();

				GfxDescriptor src_handles[] = { context.GetReadOnlyBuffer(data.clusters),
												context.GetReadWriteBuffer(data.light_counter),
												context.GetReadWriteBuffer(data.light_list),
												context.GetReadWriteBuffer(data.light_grid) };
	
				GfxDescriptor dst_handle = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_handles));
				gfx->CopyDescriptors(dst_handle, src_handles);

				Uint32 i = dst_handle.GetIndex();
				struct ClusterCullingConstants
				{
					Uint32 clusters_idx;
					Uint32 light_index_counter_idx;
					Uint32 light_index_list_idx;
					Uint32 light_grid_idx;
				} constants =
				{
					.clusters_idx = i, .light_index_counter_idx = i + 1,
					.light_index_list_idx = i + 2, .light_grid_idx = i + 3
				};

				cmd_list->SetPipelineState(clustered_culling_pso.get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(CLUSTER_SIZE_X / 16, CLUSTER_SIZE_Y / 16, CLUSTER_SIZE_Z / 1);

			}, RGPassType::Compute, RGPassFlags::None);

		struct ClusteredDeferredLightingPassData
		{
			RGTextureReadOnlyId  gbuffer_normal;
			RGTextureReadOnlyId  gbuffer_albedo;
			RGTextureReadOnlyId  gbuffer_emissive;
			RGTextureReadOnlyId  gbuffer_custom;
			RGTextureReadOnlyId  depth;
			RGTextureReadOnlyId  ambient_occlusion;
			RGTextureReadWriteId output;
			RGBufferReadOnlyId   light_grid;
			RGBufferReadOnlyId   light_list;
		};
		rendergraph.AddPass<ClusteredDeferredLightingPassData>("Clustered Deferred Lighting Pass",
			[=](ClusteredDeferredLightingPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc hdr_desc{};
				hdr_desc.width = width;
				hdr_desc.height = height;
				hdr_desc.format = GfxFormat::R16G16B16A16_FLOAT;
				hdr_desc.clear_value = GfxClearValue(0.0f, 0.0f, 0.0f, 0.0f);
				builder.DeclareTexture(RG_NAME(HDR_RenderTarget), hdr_desc);

				data.gbuffer_normal = builder.ReadTexture(RG_NAME(GBufferNormal), ReadAccess_PixelShader);
				data.gbuffer_albedo = builder.ReadTexture(RG_NAME(GBufferAlbedo), ReadAccess_PixelShader);
				data.gbuffer_emissive = builder.ReadTexture(RG_NAME(GBufferEmissive), ReadAccess_NonPixelShader);
				data.gbuffer_custom = builder.ReadTexture(RG_NAME(GBufferCustom), ReadAccess_NonPixelShader);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_PixelShader);
				data.light_grid = builder.ReadBuffer(RG_NAME(LightGrid), ReadAccess_PixelShader);
				data.light_list = builder.ReadBuffer(RG_NAME(LightList), ReadAccess_PixelShader);

				if (builder.IsTextureDeclared(RG_NAME(AmbientOcclusion)))
					data.ambient_occlusion = builder.ReadTexture(RG_NAME(AmbientOcclusion), ReadAccess_NonPixelShader);
				else data.ambient_occlusion.Invalidate();

				data.output = builder.WriteTexture(RG_NAME(HDR_RenderTarget));
			},
			[=](ClusteredDeferredLightingPassData const& data, RenderGraphContext& context, GfxCommandList* cmd_list)
			{
				GfxDevice* gfx = cmd_list->GetDevice();

				GfxDescriptor src_handles[] = { 
												context.GetReadOnlyTexture(data.gbuffer_normal), 
												context.GetReadOnlyTexture(data.gbuffer_albedo), 
												context.GetReadOnlyTexture(data.gbuffer_emissive),
												context.GetReadOnlyTexture(data.gbuffer_custom),
												context.GetReadOnlyTexture(data.depth), 
												data.ambient_occlusion.IsValid() ? context.GetReadOnlyTexture(data.ambient_occlusion) : gfxcommon::GetCommonView(GfxCommonViewType::WhiteTexture2D_SRV),
												context.GetReadWriteTexture(data.output),
												context.GetReadOnlyBuffer(data.light_list), context.GetReadOnlyBuffer(data.light_grid)
				};
				GfxDescriptor dst_handle = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_handles));
				Uint32 i = dst_handle.GetIndex();
				gfx->CopyDescriptors(dst_handle, src_handles);

				Float clear[] = { 0.0f, 0.0f, 0.0f, 0.0f };
				cmd_list->ClearUAV(context.GetTexture(*data.output), gfx->GetDescriptorGPU(i + 5),
					context.GetReadWriteTexture(data.output), clear);
				
				struct ClusteredDeferredLightingConstants
				{
					Uint32 normal_idx;
					Uint32 diffuse_idx;
					Uint32 emissive_idx;
					Uint32 custom_idx;
					Uint32 depth_idx;
					Uint32 ao_idx;
					Uint32 output_idx;
					Uint32 light_buffer_data_packed;
				} constants =
				{
					.normal_idx = i + 0, .diffuse_idx = i + 1,
					.emissive_idx = i + 2,.custom_idx = i + 3,  .depth_idx = i + 4, .ao_idx = i + 5,
					.output_idx = i + 6, .light_buffer_data_packed = PackTwoUint16ToUint32((Uint16)i + 7, (Uint16)i + 8)
				};
				ADRIA_ASSERT(i + 8 < UINT32_MAX);

				cmd_list->SetPipelineState(clustered_lighting_pso.get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
			}, RGPassType::Compute, RGPassFlags::None);
	}

	void ClusteredDeferredLightingPass::CreatePSOs()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_ClusteredDeferredLighting;
		clustered_lighting_pso = gfx->CreateComputePipelineState(compute_pso_desc);

		compute_pso_desc.CS = CS_ClusterBuilding;
		clustered_building_pso = gfx->CreateComputePipelineState(compute_pso_desc);

		compute_pso_desc.CS = CS_ClusterCulling;
		clustered_culling_pso = gfx->CreateComputePipelineState(compute_pso_desc);

	}

}



