#include "FFXDepthOfFieldPass.h"
#if defined(ADRIA_PLATFORM_WINDOWS)
#include "FidelityFXUtils.h"
#include "BlackboardData.h"
#include "PostProcessor.h"
#include "Graphics/GfxDevice.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"
#include "Math/Constants.h"
#include "Core/ConsoleManager.h"

namespace adria
{
	ADRIA_LOG_CHANNEL(PostProcessor);

	static TAutoConsoleVariable<Bool> FFXDepthOfField("r.FFXDepthOfField", true, "0 - Disabled, 1 - Enabled");

	FFXDepthOfFieldPass::FFXDepthOfFieldPass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h), ffx_interface(nullptr)
	{
		is_supported = gfx->GetBackend() == GfxBackend::D3D12 && gfx->GetCapabilities().SupportsShaderModel(SM_6_6);
		if (!is_supported)
		{
			ADRIA_LOG(WARNING, "FFXDepthOfField is only supported on D3D12 backend with SM 6.6 support");
			return;
		}

		sprintf(name_version, "FFX DoF %d.%d.%d", FFX_DOF_VERSION_MAJOR, FFX_DOF_VERSION_MINOR, FFX_DOF_VERSION_PATCH);
		ffx_interface = CreateFfxInterface(gfx, FFX_DOF_CONTEXT_COUNT);
		dof_context_desc.backendInterface = *ffx_interface;
		CreateContext();
		is_supported = true;
	}

	FFXDepthOfFieldPass::~FFXDepthOfFieldPass()
	{
		if (ffx_interface)
		{
			DestroyContext();
			DestroyFfxInterface(ffx_interface);
		}
	}

	void FFXDepthOfFieldPass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		if (!IsSupported())
		{
			ADRIA_ASSERT_MSG(false, "FFXDepthOfField is not supported on this device");
			return;
		}

		struct FFXDoFPassData
		{
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId depth;
			RGTextureReadWriteId output;
		};

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		rg.AddPass<FFXDoFPassData>(name_version,
			[=](FFXDoFPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc ffx_dof_desc = builder.GetTextureDesc(postprocessor->GetFinalResource());
				builder.DeclareTexture(RG_NAME(FFXDoFOutput), ffx_dof_desc);

				data.output = builder.WriteTexture(RG_NAME(FFXDoFOutput));
				data.input = builder.ReadTexture(postprocessor->GetFinalResource(), ReadAccess_NonPixelShader);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[=](FFXDoFPassData const& data, RenderGraphContext& ctx)
			{
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxTexture& input_texture = ctx.GetTexture(*data.input);
				GfxTexture& depth_texture = ctx.GetTexture(*data.depth);
				GfxTexture& output_texture = ctx.GetTexture(*data.output);

				Float fovx = std::min<Float>(frame_data.camera_fov * frame_data.camera_aspect_ratio, pi_div_2<Float>);
				Float conversion = 0.5f * (Float)dof_context_desc.resolution.width / sensor_size;
				Float focal_length = sensor_size / (2.0f * std::tanf(fovx * 0.5f));

				FfxDofDispatchDescription ffx_dof_dispatch_desc{};
				ffx_dof_dispatch_desc.commandList = ffxGetCommandListDX12((ID3D12CommandList*)cmd_list->GetNative());
				ffx_dof_dispatch_desc.color = GetFfxResource(input_texture);
				ffx_dof_dispatch_desc.depth = GetFfxResource(depth_texture);
				ffx_dof_dispatch_desc.output = GetFfxResource(output_texture, FFX_RESOURCE_STATE_UNORDERED_ACCESS);

				Matrix proj(frame_data.camera_proj);
				ffx_dof_dispatch_desc.cocScale = ffxDofCalculateCocScale(aperture, -focus_dist, focal_length, conversion, proj._33, proj._43, proj._34);
				ffx_dof_dispatch_desc.cocBias = ffxDofCalculateCocBias(aperture, -focus_dist, focal_length, conversion, proj._33, proj._43, proj._34);

				FfxErrorCode error_code = ffxDofContextDispatch(&dof_context, &ffx_dof_dispatch_desc);
				ADRIA_ASSERT(error_code == FFX_OK);

				cmd_list->ResetState();
			}, RGPassType::Compute);

		postprocessor->SetFinalResource(RG_NAME(FFXDoFOutput));
	}

	void FFXDepthOfFieldPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
		DestroyContext();
		CreateContext();
	}

	Bool FFXDepthOfFieldPass::IsEnabled(PostProcessor const*) const
	{
		return FFXDepthOfField.Get();
	}

	void FFXDepthOfFieldPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx(name_version, ImGuiTreeNodeFlags_None))
				{
					ImGui::Checkbox("Enable", FFXDepthOfField.GetPtr());
					if (FFXDepthOfField.Get())
					{
						ImGui::SliderFloat("Aperture", &aperture, 0.0f, 0.1f, "%.2f");
						ImGui::SliderFloat("Sensor Size", &sensor_size, 0.0f, 0.1f, "%.2f");
						ImGui::SliderFloat("Focus Distance", &focus_dist, 0.1f, 1000.0f, "%.2f");

						Bool recreate_context = false;

						recreate_context |= ImGui::SliderInt("Quality", &quality, 1, 50);
						recreate_context |= ImGui::SliderFloat("Blur Size Limit", &coc_limit, 0.0f, 1.0f);
						recreate_context |= ImGui::Checkbox("Enable Kernel Ring Merging", &enable_ring_merge);

						if (recreate_context)
						{
							DestroyContext();
							CreateContext();
						}
					}
					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_DepthOfField);
	}

	void FFXDepthOfFieldPass::CreateContext()
	{
		dof_context_desc.flags = FFX_DOF_REVERSE_DEPTH;
		if (!enable_ring_merge)
		{
			dof_context_desc.flags |= FFX_DOF_DISABLE_RING_MERGE;
		}
		
		dof_context_desc.resolution.width = width;
		dof_context_desc.resolution.height = height;
		dof_context_desc.quality = static_cast<Uint32>(quality);
		dof_context_desc.cocLimitFactor = coc_limit;
		dof_context_desc.backendInterface.device = gfx->GetNative();

		FfxErrorCode error_code = ffxDofContextCreate(&dof_context, &dof_context_desc);
		ADRIA_ASSERT(error_code == FFX_OK);
	}

	void FFXDepthOfFieldPass::DestroyContext()
	{
		gfx->WaitForGPU();
		ffxDofContextDestroy(&dof_context);
	}

}

#endif 

