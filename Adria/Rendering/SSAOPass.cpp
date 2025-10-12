#include "SSAOPass.h"
#include "Components.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineState.h"
#include "Math/Packing.h"
#include "RenderGraph/RenderGraph.h"
#include "Utilities/Random.h"
#include "Core/ConsoleManager.h"
#include "Editor/GUICommand.h"

using namespace DirectX;


namespace adria
{	
	enum SSAOResolution
	{
		SSAOResolution_Full = 0,
		SSAOResolution_Half = 1,
		SSAOResolution_Quarter = 2
	};

	static TAutoConsoleVariable<Float> SSAOPower("r.SSAO.Power", 1.5f, "Controls the power of SSAO");
	static TAutoConsoleVariable<Float> SSAORadius("r.SSAO.Radius", 1.0f, "Controls the radius of SSAO");
	static TAutoConsoleVariable<Int>   SSAOResolution("r.SSAO.Resolution", SSAOResolution_Full, "Sets the resolution mode for SSAO: 0 - Full resolution, 1 - Half resolution, 2 - Quarter resolution");

	SSAOPass::SSAOPass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h), ssao_random_texture(nullptr), blur_pass(gfx)
	{
		CreatePSO();
		RealRandomGenerator rand_float(0.0f, 1.0f);
		for (Uint32 i = 0; i < ARRAYSIZE(ssao_kernel); i++)
		{
			Vector4 offset(2 * rand_float() - 1, 2 * rand_float() - 1, rand_float(), 0.0f);
			offset.Normalize();
			offset *= rand_float();
			ssao_kernel[i] = offset;
		}
		SSAOResolution->AddOnChanged(ConsoleVariableDelegate::CreateLambda([&](IConsoleVariable* cvar) { OnResize(width, height); }));
	}
	SSAOPass::~SSAOPass() = default;

	void SSAOPass::AddPass(RenderGraph& rg)
	{
		RG_SCOPE(rg, "SSAO");

		struct SSAOPassData
		{
			RGTextureReadOnlyId gbuffer_normal;
			RGTextureReadOnlyId depth;
			RGTextureReadWriteId output;
		};

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		rg.AddPass<SSAOPassData>("SSAO Pass",
			[=](SSAOPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc ssao_desc{};
				ssao_desc.format = GfxFormat::R8_UNORM;
				ssao_desc.width = width >> SSAOResolution.Get();
				ssao_desc.height = height >> SSAOResolution.Get();

				builder.DeclareTexture(RG_NAME(SSAO_Output), ssao_desc);
				data.output = builder.WriteTexture(RG_NAME(SSAO_Output));
				data.gbuffer_normal = builder.ReadTexture(RG_NAME(GBufferNormal), ReadAccess_NonPixelShader);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[&](SSAOPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.depth),
					ctx.GetReadOnlyTexture(data.gbuffer_normal),
					ssao_random_texture_srv,
					ctx.GetReadWriteTexture(data.output)
				};
				GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_descriptors));
				gfx->CopyDescriptors(dst_descriptor, src_descriptors);
				Uint32 const i = dst_descriptor.GetIndex();

				Uint32 ssao_width  = width >> SSAOResolution.Get();
				Uint32 ssao_height = height >> SSAOResolution.Get();

				struct SSAOConstants
				{
					Uint32 ssao_params_packed;
					Uint32 resolution_factor;
					Float  noise_scale_x;
					Float  noise_scale_y;
		
					Uint32   depth_idx;
					Uint32   normal_idx;
					Uint32   noise_idx;
					Uint32   output_idx;
				} constants = 
				{
					.ssao_params_packed = PackTwoFloatsToUint32(SSAORadius.Get(),SSAOPower.Get()), .resolution_factor = (Uint32)SSAOResolution.Get(),
					.noise_scale_x = ssao_width * 1.0f / NOISE_DIM, .noise_scale_y = ssao_height * 1.0f / NOISE_DIM,
					.depth_idx = i, .normal_idx = i + 1, .noise_idx = i + 2, .output_idx = i + 3
				};

				cmd_list->SetPipelineState(ssao_pso.get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->SetRootCBV(2, ssao_kernel);
				cmd_list->Dispatch(DivideAndRoundUp(ssao_width, 16), DivideAndRoundUp(ssao_height, 16), 1);

			}, RGPassType::AsyncCompute);

		blur_pass.SetAsyncCompute(true);
		blur_pass.AddPass(rg, RG_NAME(SSAO_Output), RG_NAME(AmbientOcclusion), " SSAO");
	}

	void SSAOPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("SSAO", ImGuiTreeNodeFlags_None))
				{
					ImGui::SliderFloat("Power", SSAOPower.GetPtr(), 1.0f, 16.0f);
					ImGui::SliderFloat("Radius", SSAORadius.GetPtr(), 0.5f, 4.0f);

					if (ImGui::Combo("SSAO Resolution", SSAOResolution.GetPtr(), "Full\0Half\0Quarter\0", 3))
					{
						OnResize(width, height);
					}
					ImGui::TreePop();
					ImGui::Separator();
				}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_AO);
	}

	void SSAOPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	void SSAOPass::OnSceneInitialized()
	{
		RealRandomGenerator rand_float{ 0.0f, 1.0f };
		std::vector<Float> random_texture_data;
		for (Int32 i = 0; i < 8 * 8; i++)
		{
			random_texture_data.push_back(rand_float()); 
			random_texture_data.push_back(rand_float());
			random_texture_data.push_back(0.0f);
			random_texture_data.push_back(1.0f);
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

		ssao_random_texture = gfx->CreateTexture(noise_desc, init_data);
		ssao_random_texture->SetName("SSAO Random Texture");
		ssao_random_texture_srv = gfx->CreateTextureSRV(ssao_random_texture.get());
	}

	void SSAOPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_Ssao;
		ssao_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

}

