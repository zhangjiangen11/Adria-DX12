#include "DLSS3Pass.h"
#if defined(ADRIA_DLSS3_SUPPORTED)
#define NV_WINDOWS
#include "nvsdk_ngx_helpers.h"
#include "BlackboardData.h"
#include "PostProcessor.h"
#include "Graphics/GfxDevice.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"
#include "Core/ConsoleManager.h"

namespace adria
{
	ADRIA_LOG_CHANNEL(PostProcessor);

	static constexpr Char const* project_guid = "a0f57b54-1daf-4934-90ae-c4035c19df04";
	namespace
	{
		void NVSDK_CONV DLSS3Log(Char const* message, NVSDK_NGX_Logging_Level logging_level, NVSDK_NGX_Feature source_component)
		{
		}
	}

	DLSS3Pass::DLSS3Pass(GfxDevice* gfx, Uint32 w, Uint32 h) 
		: gfx(gfx), display_width(), display_height(), render_width(), render_height()
	{
		is_supported = gfx->GetBackend() == GfxBackend::D3D12 && gfx->GetVendor() == GfxVendor::Nvidia;
		if(!is_supported)
		{
			ADRIA_LOG(WARNING, "DLSS 3.5 is only supported on D3D12 backend and NVIDIA GPUs");
			return;
		}

		sprintf(name_version, "DLSS 3.5");
		is_supported = InitializeNVSDK_NGX();
	}

	DLSS3Pass::~DLSS3Pass()
	{
		if (ngx_parameters)
		{
			gfx->WaitForGPU();
			NVSDK_NGX_Result result = NVSDK_NGX_D3D12_DestroyParameters(ngx_parameters);
			ADRIA_ASSERT(NVSDK_NGX_SUCCEED(result));
			result = NVSDK_NGX_D3D12_Shutdown1((ID3D12Device*)gfx->GetNative());
			ADRIA_ASSERT(NVSDK_NGX_SUCCEED(result));
		}
	}

	void DLSS3Pass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		if (!IsSupported())
		{
			ADRIA_ASSERT_MSG(false, "DLSS is not supported on this device");
			return;
		}

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct DLSS3PassData
		{
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId depth;
			RGTextureReadOnlyId velocity;
			RGTextureReadOnlyId exposure;
			RGTextureReadWriteId output;
		};

		rg.AddPass<DLSS3PassData>(name_version,
			[=](DLSS3PassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc dlss3_desc{};
				dlss3_desc.format = GfxFormat::R16G16B16A16_FLOAT;
				dlss3_desc.width = display_width;
				dlss3_desc.height = display_height;
				dlss3_desc.clear_value = GfxClearValue(0.0f, 0.0f, 0.0f, 0.0f);
				builder.DeclareTexture(RG_NAME(DLSS3Output), dlss3_desc);

				data.output = builder.WriteTexture(RG_NAME(DLSS3Output));
				data.input = builder.ReadTexture(postprocessor->GetFinalResource(), ReadAccess_NonPixelShader);
				data.velocity = builder.ReadTexture(RG_NAME(VelocityBuffer), ReadAccess_NonPixelShader);
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
			},
			[=](DLSS3PassData const& data, RenderGraphContext& ctx)
			{
				GfxCommandList* cmd_list = ctx.GetCommandList();
				if (needs_create)
				{
					ReleaseDLSS();
					CreateDLSS(cmd_list);
				}

				GfxTexture& input_texture = ctx.GetTexture(*data.input);
				GfxTexture& velocity_texture = ctx.GetTexture(*data.velocity);
				GfxTexture& depth_texture = ctx.GetTexture(*data.depth);
				GfxTexture& output_texture = ctx.GetTexture(*data.output);

				NVSDK_NGX_D3D12_DLSS_Eval_Params dlss_eval_params{};
				dlss_eval_params.Feature.pInColor = (ID3D12Resource*)input_texture.GetNative();
				dlss_eval_params.Feature.pInOutput = (ID3D12Resource*)output_texture.GetNative();
				dlss_eval_params.Feature.InSharpness = sharpness;
				dlss_eval_params.pInDepth = (ID3D12Resource*)depth_texture.GetNative();
				dlss_eval_params.pInMotionVectors = (ID3D12Resource*)velocity_texture.GetNative();
				dlss_eval_params.InMVScaleX = (Float)render_width;
				dlss_eval_params.InMVScaleY = (Float)render_height;

				dlss_eval_params.pInExposureTexture = nullptr;
				dlss_eval_params.InExposureScale = 1.0f;

				dlss_eval_params.InJitterOffsetX = frame_data.camera_jitter_x;
				dlss_eval_params.InJitterOffsetY = frame_data.camera_jitter_y;
				dlss_eval_params.InReset = false;
				dlss_eval_params.InRenderSubrectDimensions = { render_width, render_height };

				NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT((ID3D12GraphicsCommandList*)cmd_list->GetNative(), dlss_feature, ngx_parameters, &dlss_eval_params);
				ADRIA_ASSERT(NVSDK_NGX_SUCCEED(result));

				cmd_list->ResetState();
			}, RGPassType::Compute);

		postprocessor->SetFinalResource(RG_NAME(DLSS3Output));
	}

	Bool DLSS3Pass::IsEnabled(PostProcessor const*) const
	{
		return true;
	}

	void DLSS3Pass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx(name_version, ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Combo("Performance Quality", (Int*)&perf_quality, "Max Performance\0Balanced\0Max Quality\0Ultra Performance\0", 4))
					{
						RecreateRenderResolution();
						needs_create = true;
					}
					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_Upscaler);

	}

	Bool DLSS3Pass::InitializeNVSDK_NGX()
	{
		ID3D12Device* device = (ID3D12Device*)gfx->GetNative();

		static const Wchar* dll_paths[] = 
		{ 
		SOLUTION_DIR_W L"\\External\\DLSS\\lib\\dev",
		SOLUTION_DIR_W L"\\External\\DLSS\\lib\\rel",
		};

		NVSDK_NGX_FeatureCommonInfo feature_common_info{};
		feature_common_info.LoggingInfo.LoggingCallback = DLSS3Log;
		feature_common_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
		feature_common_info.LoggingInfo.DisableOtherLoggingSinks = true;
		feature_common_info.PathListInfo.Path = dll_paths;
		feature_common_info.PathListInfo.Length = NVSDK_NGX_ARRAY_LEN(dll_paths);

		NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
			project_guid,
			NVSDK_NGX_ENGINE_TYPE_CUSTOM,
			"1.0",
			L".",
			device, &feature_common_info);

		result = NVSDK_NGX_D3D12_GetCapabilityParameters(&ngx_parameters);
		if (NVSDK_NGX_FAILED(result))
		{
			return false;
		}

		Int needs_updated_driver = 0;
		Uint min_driver_version_major = 0;
		Uint min_driver_version_minor = 0;
		NVSDK_NGX_Result result_updated_driver = ngx_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_updated_driver);
		NVSDK_NGX_Result result_min_driver_version_major = ngx_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_driver_version_major);
		NVSDK_NGX_Result result_min_driver_version_minor = ngx_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_driver_version_minor);
		if (NVSDK_NGX_SUCCEED(result_updated_driver))
		{
			if (needs_updated_driver)
			{
				if (NVSDK_NGX_SUCCEED(result_min_driver_version_major) &&
					NVSDK_NGX_SUCCEED(result_min_driver_version_minor))
				{
					ADRIA_LOG(WARNING, "Nvidia DLSS cannot be loaded due to outdated driver, min driver version: %ul.%ul", min_driver_version_major, min_driver_version_minor);
					return false;
				}
				ADRIA_LOG(WARNING, "Nvidia DLSS cannot be loaded due to outdated driver");
			}
		}

		Int dlss_available = 0;
		result = ngx_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_available);
		if (NVSDK_NGX_FAILED(result) || !dlss_available)
		{
			return false;
		}

		return true;
	}

	void DLSS3Pass::RecreateRenderResolution()
	{
		Uint optimal_width;
		Uint optimal_height;
		Uint max_width;
		Uint max_height;
		Uint min_width;
		Uint min_height;
		Float sharpness;
		NGX_DLSS_GET_OPTIMAL_SETTINGS(ngx_parameters, display_width, display_height, perf_quality, 
									  &optimal_width, &optimal_height, &max_width, &max_height, &min_width, &min_height, &sharpness);
		ADRIA_ASSERT(optimal_width != 0 && optimal_height != 0);
		render_width = optimal_width;
		render_height = optimal_height;
		BroadcastRenderResolutionChanged(render_width, render_height);
	}

	void DLSS3Pass::CreateDLSS(GfxCommandList* cmd_list)
	{
		if (!IsSupported())
		{
			return;
		}
		ADRIA_ASSERT(needs_create);

		NVSDK_NGX_DLSS_Create_Params dlss_create_params{};
		dlss_create_params.Feature.InWidth = render_width;
		dlss_create_params.Feature.InHeight = render_height;
		dlss_create_params.Feature.InTargetWidth = display_width;
		dlss_create_params.Feature.InTargetHeight = display_height;
		dlss_create_params.Feature.InPerfQualityValue = perf_quality;
		dlss_create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
			NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
			NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
			NVSDK_NGX_DLSS_Feature_Flags_DoSharpening |
			NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
		dlss_create_params.InEnableOutputSubrects = false;

		NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT((ID3D12GraphicsCommandList*)cmd_list->GetNative(), 0, 0, &dlss_feature, ngx_parameters, &dlss_create_params);
		ADRIA_ASSERT(NVSDK_NGX_SUCCEED(result));
		cmd_list->GlobalBarrier(GfxResourceState::ComputeUAV, GfxResourceState::ComputeUAV);
		needs_create = false;
	}

	void DLSS3Pass::ReleaseDLSS()
	{
		if (dlss_feature)
		{
			gfx->WaitForGPU();
			NVSDK_NGX_D3D12_ReleaseFeature(dlss_feature);
			dlss_feature = nullptr;
			}

}
#endif

