#include "RayTracedAmbientOcclusionPass.h"
#include "BlackboardData.h"
#include "ShaderManager.h"
#include "Graphics/GfxShaderKey.h"
#include "Graphics/GfxStateObject.h"
#include "Graphics/GfxPipelineState.h"
#include "Graphics/D3D12/D3D12Device.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"

namespace adria
{
	
	RayTracedAmbientOcclusionPass::RayTracedAmbientOcclusionPass(GfxDevice* gfx, Uint32 width, Uint32 height)
		: d3d12_gfx((D3D12Device*)gfx), width(width), height(height), blur_pass(gfx)
	{
		is_supported = d3d12_gfx->GetCapabilities().SupportsRayTracing();
		if (IsSupported())
		{
			CreatePSO();
			CreateStateObject();
			ShaderManager::GetLibraryRecompiledEvent().AddMember(&RayTracedAmbientOcclusionPass::OnLibraryRecompiled, *this);
		}
	}

	RayTracedAmbientOcclusionPass::~RayTracedAmbientOcclusionPass() = default;

	void RayTracedAmbientOcclusionPass::AddPass(RenderGraph& rg)
	{
		if (!IsSupported()) return;
		RG_SCOPE(rg, "RTAO");

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		struct RayTracedAmbientOcclusionPassData
		{
			RGTextureReadOnlyId depth;
			RGTextureReadOnlyId normal;
			RGTextureReadWriteId output;
		};

		rg.AddPass<RayTracedAmbientOcclusionPassData>("Ray Traced Ambient Occlusion Pass",
			[=](RayTracedAmbientOcclusionPassData& data, RGBuilder& builder)
			{
				RGTextureDesc desc{};
				desc.width = width;
				desc.height = height;
				desc.format = GfxFormat::R8_UNORM;
				builder.DeclareTexture(RG_NAME(RTAO_Output), desc);

				data.output = builder.WriteTexture(RG_NAME(RTAO_Output));
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
				data.normal = builder.ReadTexture(RG_NAME(GBufferNormal), ReadAccess_NonPixelShader);
			},
			[=](RayTracedAmbientOcclusionPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth),
					ctx.GetReadOnlyTexture(data.normal),
					ctx.GetReadWriteTexture(data.output)
				};
				GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_descriptors));
				gfx->CopyDescriptors(dst_descriptor, src_descriptors);
				Uint32 const i = dst_descriptor.GetIndex();

				struct RayTracedAmbientOcclusionConstants
				{
					Uint32  depth_idx;
					Uint32  gbuf_normals_idx;
					Uint32  output_idx;
					Float   ao_radius;
					Float   ao_power;
				} constants =
				{
					.depth_idx = i + 0, .gbuf_normals_idx = i + 1, .output_idx = i + 2,
					.ao_radius = params.radius, .ao_power = pow(2.f, params.power_log)
				};

				auto& table = cmd_list->SetStateObject(ray_traced_ambient_occlusion_so.get());
				table.SetRayGenShader("RTAO_RayGen");
				table.AddMissShader("RTAO_Miss", 0);
				table.AddHitGroup("RTAOAnyHitGroup", 0);

				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->DispatchRays(width, height);

			}, RGPassType::Compute, RGPassFlags::None);

		struct RTAOFilterPassData
		{
			RGTextureReadOnlyId depth;
			RGTextureReadOnlyId input;
			RGTextureReadWriteId output;
		};

		rg.AddPass<RTAOFilterPassData>("RTAO Filter Pass",
			[=](RTAOFilterPassData& data, RGBuilder& builder)
			{
				RGTextureDesc desc{};
				desc.width = width;
				desc.height = height;
				desc.format = GfxFormat::R8_UNORM;
				builder.DeclareTexture(RG_NAME(AmbientOcclusion), desc);

				data.output = builder.WriteTexture(RG_NAME(AmbientOcclusion));
				data.input = builder.ReadTexture(RG_NAME(RTAO_Output), ReadAccess_NonPixelShader);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[=](RTAOFilterPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth),
					ctx.GetReadOnlyTexture(data.input),
					ctx.GetReadWriteTexture(data.output)
				};
				GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_descriptors));
				gfx->CopyDescriptors(dst_descriptor, src_descriptors);
				Uint32 const i = dst_descriptor.GetIndex();

				struct RTAOFilterIndices
				{
					Uint32  depth_idx;
					Uint32  input_idx;
					Uint32  output_idx;
				} indices =
				{
					.depth_idx = i + 0, .input_idx = i + 1, .output_idx = i + 2
				};

				Float distance_kernel[6];
				for (Uint64 i = 0; i < 6; ++i)
				{
					distance_kernel[i] = (Float)exp(-Float(i * i) / (2.f * params.filter_distance_sigma * params.filter_distance_sigma));
				}

				struct RTAOFilterConstants
				{
					Float filter_width;
					Float filter_height;
					Float filter_distance_sigma;
					Float filter_depth_sigma;
					Float filter_dist_kernel0;
					Float filter_dist_kernel1;
					Float filter_dist_kernel2;
					Float filter_dist_kernel3;
					Float filter_dist_kernel4;
					Float filter_dist_kernel5;
				} constants =
				{
					.filter_width = (Float)width, .filter_height = (Float)height, .filter_distance_sigma = params.filter_distance_sigma, .filter_depth_sigma = params.filter_depth_sigma,
					.filter_dist_kernel0 = distance_kernel[0], .filter_dist_kernel1 = distance_kernel[1],
					.filter_dist_kernel2 = distance_kernel[2], .filter_dist_kernel3 = distance_kernel[3],
					.filter_dist_kernel4 = distance_kernel[4], .filter_dist_kernel5 = distance_kernel[5],
				};

				cmd_list->SetPipelineState(rtao_filter_pso->Get());

				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, indices);
				cmd_list->SetRootCBV(2, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 32), DivideAndRoundUp(height, 32), 1);

			}, RGPassType::Compute, RGPassFlags::None);
	}

	void RayTracedAmbientOcclusionPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("RTAO", ImGuiTreeNodeFlags_None))
				{
					ImGui::SliderFloat("Radius", &params.radius, 1.0f, 32.0f);
					ImGui::SliderFloat("Power (log2)", &params.power_log, -10.0f, 10.0f);
					ImGui::SliderFloat("Filter Distance Sigma", &params.filter_distance_sigma, 0.0f, 25.0f);
					ImGui::SliderFloat("Filter Depth Sigma", &params.filter_depth_sigma, 0.0f, 1.0f);
					ImGui::TreePop();
					ImGui::Separator();
				}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_AO);
	}

	void RayTracedAmbientOcclusionPass::OnResize(Uint32 w, Uint32 h)
	{
		if (!IsSupported()) return;
		width = w, height = h;
	}

	Bool RayTracedAmbientOcclusionPass::IsSupported() const
	{
		return is_supported;
	}

	void RayTracedAmbientOcclusionPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_RTAOFilter;
		rtao_filter_pso = d3d12_gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

	void RayTracedAmbientOcclusionPass::CreateStateObject()
	{
		GfxShader const& rtao_blob = SM_GetGfxShader(LIB_AmbientOcclusion);
		GfxStateObjectBuilder rtao_state_object_builder(5);
		{
			D3D12_DXIL_LIBRARY_DESC	dxil_lib_desc{};
			dxil_lib_desc.DXILLibrary.BytecodeLength = rtao_blob.GetSize();
			dxil_lib_desc.DXILLibrary.pShaderBytecode = rtao_blob.GetData();
			dxil_lib_desc.NumExports = 0;
			dxil_lib_desc.pExports = nullptr;
			rtao_state_object_builder.AddSubObject(dxil_lib_desc);

			D3D12_RAYTRACING_SHADER_CONFIG rtao_shader_config{};
			rtao_shader_config.MaxPayloadSizeInBytes = 4;
			rtao_shader_config.MaxAttributeSizeInBytes = D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES;
			rtao_state_object_builder.AddSubObject(rtao_shader_config);

			D3D12_GLOBAL_ROOT_SIGNATURE global_root_sig{};
			global_root_sig.pGlobalRootSignature = d3d12_gfx->GetCommonRootSignature();
			rtao_state_object_builder.AddSubObject(global_root_sig);

			D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{};
			pipeline_config.MaxTraceRecursionDepth = 1;
			rtao_state_object_builder.AddSubObject(pipeline_config);

			D3D12_HIT_GROUP_DESC anyhit_group{};
			anyhit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
			anyhit_group.AnyHitShaderImport = L"RTAO_AnyHit";
			anyhit_group.HitGroupExport = L"RTAOAnyHitGroup";
			rtao_state_object_builder.AddSubObject(anyhit_group);
		}
		ray_traced_ambient_occlusion_so.reset(rtao_state_object_builder.CreateStateObject(d3d12_gfx));
	}

	void RayTracedAmbientOcclusionPass::OnLibraryRecompiled(GfxShaderKey const& key)
	{
		if (key.GetShaderID() == LIB_AmbientOcclusion) CreateStateObject();
	}

}


