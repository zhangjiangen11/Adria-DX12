#include "D3D12_FSR2Pass.h"
#include "FidelityFX/gpu/fsr2/ffx_fsr2_resources.h"
#include "FidelityFXUtils.h"
#include "BlackboardData.h"
#include "PostProcessor.h"
#include "Graphics/GfxDevice.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"
#include "Core/ConsoleManager.h"
#include "Utilities/StringConversions.h"

namespace adria
{
	ADRIA_LOG_CHANNEL(PostProcessor);

	namespace
	{
		void FSR2Log(FfxMsgType type, const Wchar* message)
		{
			std::string msg = ToString(message);
			switch (type)
			{
			case FFX_MESSAGE_TYPE_WARNING:
				ADRIA_LOG(WARNING, msg.c_str());
				break;
			case FFX_MESSAGE_TYPE_ERROR:
				ADRIA_LOG(ERROR, msg.c_str());
				break;
			default:
				break;
			}
		}
	}
	D3D12_FSR2Pass::D3D12_FSR2Pass(GfxDevice* _gfx, Uint32 w, Uint32 h) : gfx(_gfx), display_width(w), display_height(h), render_width(), render_height()
	{
		sprintf(name_version, "FSR %d.%d.%d", FFX_FSR2_VERSION_MAJOR, FFX_FSR2_VERSION_MINOR, FFX_FSR2_VERSION_PATCH);
		ffx_interface = CreateFfxInterface(gfx, FFX_FSR2_CONTEXT_COUNT);
		fsr2_context_desc.backendInterface = *ffx_interface;
		RecreateRenderResolution();
		CreateContext();
	}

	D3D12_FSR2Pass::~D3D12_FSR2Pass()
	{
		DestroyContext();
		DestroyFfxInterface(ffx_interface);
	}

	void D3D12_FSR2Pass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		if (recreate_context)
		{
			DestroyContext();
			CreateContext();
		}

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct FSR2PassData
		{
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId depth;
			RGTextureReadOnlyId velocity;
			RGTextureReadOnlyId exposure;
			RGTextureReadWriteId output;
		};

		rg.AddPass<FSR2PassData>(name_version,
			[=](FSR2PassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc fsr2_desc{};
				fsr2_desc.format = GfxFormat::R16G16B16A16_FLOAT;
				fsr2_desc.width = display_width;
				fsr2_desc.height = display_height;
				fsr2_desc.clear_value = GfxClearValue(0.0f, 0.0f, 0.0f, 0.0f);
				builder.DeclareTexture(RG_NAME(FSR2Output), fsr2_desc);

				data.output = builder.WriteTexture(RG_NAME(FSR2Output));
				data.input = builder.ReadTexture(postprocessor->GetFinalResource(), ReadAccess_NonPixelShader);
				data.velocity = builder.ReadTexture(RG_NAME(VelocityBuffer), ReadAccess_NonPixelShader);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[=](FSR2PassData const& data, RenderGraphContext& ctx)
			{
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxTexture& input_texture = ctx.GetTexture(*data.input);
				GfxTexture& velocity_texture = ctx.GetTexture(*data.velocity);
				GfxTexture& depth_texture = ctx.GetTexture(*data.depth);
				GfxTexture& output_texture = ctx.GetTexture(*data.output);

				FfxFsr2DispatchDescription dispatch_desc{};
				dispatch_desc.commandList = ffxGetCommandListDX12((ID3D12CommandList*)cmd_list->GetNative());
				dispatch_desc.color = GetFfxResource(input_texture);
				dispatch_desc.depth = GetFfxResource(depth_texture);
				dispatch_desc.motionVectors = GetFfxResource(velocity_texture);
				dispatch_desc.exposure = GetFfxResource();
				dispatch_desc.reactive = GetFfxResource();
				dispatch_desc.transparencyAndComposition = GetFfxResource();
				dispatch_desc.output = GetFfxResource(output_texture, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
				dispatch_desc.jitterOffset.x = frame_data.camera_jitter_x;
				dispatch_desc.jitterOffset.y = frame_data.camera_jitter_y;
				dispatch_desc.motionVectorScale.x = (Float)render_width;
				dispatch_desc.motionVectorScale.y = (Float)render_height;
				dispatch_desc.reset = false;
				dispatch_desc.enableSharpening = true;
				dispatch_desc.sharpness = sharpness;
				dispatch_desc.frameTimeDelta = frame_data.delta_time;
				dispatch_desc.preExposure = 1.0f;
				dispatch_desc.renderSize.width = render_width;
				dispatch_desc.renderSize.height = render_height;
				dispatch_desc.cameraFar = frame_data.camera_far;
				dispatch_desc.cameraNear = frame_data.camera_near;
				dispatch_desc.cameraFovAngleVertical = frame_data.camera_fov;

				FfxErrorCode error_code = ffxFsr2ContextDispatch(&fsr2_context, &dispatch_desc);
				ADRIA_ASSERT(error_code == FFX_OK);

				cmd_list->ResetState();
			}, RGPassType::Compute);

		postprocessor->SetFinalResource(RG_NAME(FSR2Output));
	}

	Bool D3D12_FSR2Pass::IsEnabled(PostProcessor const*) const
	{
		return true;
	}

	void D3D12_FSR2Pass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx(name_version, ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Combo("Quality Mode", (Int32*)&fsr2_quality_mode, "Custom\0Quality (1.5x)\0Balanced (1.7x)\0Performance (2.0x)\0Ultra Performance (3.0x)\0", 5))
					{
						RecreateRenderResolution();
						recreate_context = true;
					}

					if (fsr2_quality_mode == 0)
					{
						if (ImGui::SliderFloat("Upscale Ratio", &custom_upscale_ratio, 1.0, 3.0))
						{
							RecreateRenderResolution();
							recreate_context = true;
						}
					}
					ImGui::SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f, "%.2f");
					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_Upscaler);
	}

	void D3D12_FSR2Pass::CreateContext()
	{
		ADRIA_ASSERT(recreate_context);

		fsr2_context_desc.fpMessage = FSR2Log;
		fsr2_context_desc.maxRenderSize.width = render_width;
		fsr2_context_desc.maxRenderSize.height = render_height;
		fsr2_context_desc.displaySize.width = display_width;
		fsr2_context_desc.displaySize.height = display_height;
		fsr2_context_desc.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR2_ENABLE_AUTO_EXPOSURE | FFX_FSR2_ENABLE_DEPTH_INVERTED;

		FfxErrorCode error_code = ffxFsr2ContextCreate(&fsr2_context, &fsr2_context_desc);
		ADRIA_ASSERT(error_code == FFX_OK);
		recreate_context = false;
	}

	void D3D12_FSR2Pass::DestroyContext()
	{
		gfx->WaitForGPU();
		ffxFsr2ContextDestroy(&fsr2_context);
	}

	void D3D12_FSR2Pass::RecreateRenderResolution()
	{
		Float upscale_ratio = (fsr2_quality_mode == 0 ? custom_upscale_ratio : ffxFsr2GetUpscaleRatioFromQualityMode(fsr2_quality_mode));
		render_width = (Uint32)((Float)display_width / upscale_ratio);
		render_height = (Uint32)((Float)display_height / upscale_ratio);
		BroadcastRenderResolutionChanged(render_width, render_height);
	}
}