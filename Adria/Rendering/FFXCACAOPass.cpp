#include "FFXCACAOPass.h"
#include "FidelityFXUtils.h"
#include "BlackboardData.h"
#include "Graphics/GfxDevice.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"

namespace adria
{
	namespace
	{
		struct CacaoPreset 
		{
			Bool use_downsampled_ssao;
			FfxCacaoSettings cacao_settings;
		};
		std::vector<std::string> FfxCacaoPresetNames = 
		{
			"Native - Adaptive Quality",
			"Native - High Quality",
			"Native - Medium Quality",
			"Native - Low Quality",
			"Native - Lowest Quality",
			"Downsampled - Adaptive Quality",
			"Downsampled - High Quality",
			"Downsampled - Medium Quality",
			"Downsampled - Low Quality",
			"Downsampled - Lowest Quality",
			"Custom"
		};
		const CacaoPreset FfxCacaoPresets[] = 
		{
			// Native - Adaptive Quality
			{
				/* useDownsampledSsao */ false,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_HIGHEST,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 2,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 5.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.1f,
			}
		},
			// Native - High Quality
			{
				/* useDownsampledSsao */ false,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_HIGH,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 2,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 5.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.1f,
			}
		},
			// Native - Medium Quality
			{
				/* useDownsampledSsao */ false,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_MEDIUM,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 2,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 5.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.1f,
			}
		},
			// Native - Low Quality
			{
				/* useDownsampledSsao */ false,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_LOW,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 6,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 5.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.1f,
			}
		},
			// Native - Lowest Quality
			{
				/* useDownsampledSsao */ false,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_LOWEST,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 6,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 5.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.1f,
			}
		},
			// Downsampled - Highest Quality
			{
				/* useDownsampledSsao */ true,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_HIGHEST,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 2,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 5.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.1f,
			}
		},
			// Downsampled - High Quality
			{
				/* useDownsampledSsao */ true,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_HIGH,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 2,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 5.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.1f,
			}
		},
			// Downsampled - Medium Quality
			{
				/* useDownsampledSsao */ true,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_MEDIUM,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 3,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 5.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.2f,
			}
		},
			// Downsampled - Low Quality
			{
				/* useDownsampledSsao */ true,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_LOW,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 6,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 8.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.8f,
			}
		},
			// Downsampled - Lowest Quality
			{
				/* useDownsampledSsao */ true,
				{
				/* radius                            */ 1.2f,
				/* shadowMultiplier                  */ 1.0f,
				/* shadowPower                       */ 1.50f,
				/* shadowClamp                       */ 0.98f,
				/* horizonAngleThreshold             */ 0.06f,
				/* fadeOutFrom                       */ 20.0f,
				/* fadeOutTo                         */ 40.0f,
				/* qualityLevel                      */ FFX_CACAO_QUALITY_LOWEST,
				/* adaptiveQualityLimit              */ 0.75f,
				/* blurPassCount                     */ 6,
				/* sharpness                         */ 0.98f,
				/* temporalSupersamplingAngleOffset  */ 0.0f,
				/* temporalSupersamplingRadiusOffset */ 0.0f,
				/* detailShadowStrength              */ 0.5f,
				/* generateNormals                   */ false,
				/* bilateralSigmaSquared             */ 8.0f,
				/* bilateralSimilarityDistanceSigma  */ 0.8f,
			}
		}
		};
	}

	ADRIA_LOG_CHANNEL(PostProcessor);
	
	FFXCACAOPass::FFXCACAOPass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h), ffx_interface(nullptr)
	{
		is_supported = gfx->GetBackend() == GfxBackend::D3D12 && gfx->GetCapabilities().SupportsShaderModel(SM_6_6);
		if (!is_supported)
		{
			ADRIA_LOG(WARNING, "FFXCACAO is only supported on D3D12 backend with SM 6.6 support");
			return;
		}

		sprintf(name_version, "FFX CACAO %d.%d.%d", FFX_CACAO_VERSION_MAJOR, FFX_CACAO_VERSION_MINOR, FFX_CACAO_VERSION_PATCH);
		ffx_interface = CreateFfxInterface(gfx, FFX_CACAO_CONTEXT_COUNT * 2);

		cacao_context_desc.backendInterface = *ffx_interface;
		CreateContext();

		preset_id = 2;
		cacao_settings = FfxCacaoPresets[preset_id].cacao_settings;
		use_downsampled_ssao = FfxCacaoPresets[preset_id].use_downsampled_ssao;
	}

	FFXCACAOPass::~FFXCACAOPass()
	{
		if (ffx_interface)
		{
			DestroyContext();
			DestroyFfxInterface(ffx_interface);
		}
	}

	void FFXCACAOPass::AddPass(RenderGraph& rg)
	{
		if (!IsSupported())
		{
			ADRIA_ASSERT_MSG(false, "FFXCACAO is not supported on this device");
			return;
		}

		RG_SCOPE(rg, "CACAO");

		struct FFXCACAOPassData
		{
			RGTextureReadOnlyId gbuffer_normal;
			RGTextureReadOnlyId depth;
			RGTextureReadWriteId output;
		};

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		rg.AddPass<FFXCACAOPassData>(name_version,
			[=](FFXCACAOPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc cacao_desc{};
				cacao_desc.format = GfxFormat::R8_UNORM;
				cacao_desc.width = width;
				cacao_desc.height = height;
				builder.DeclareTexture(RG_NAME(AmbientOcclusion), cacao_desc);

				data.output = builder.WriteTexture(RG_NAME(AmbientOcclusion));
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
				data.gbuffer_normal = builder.ReadTexture(RG_NAME(GBufferNormal), ReadAccess_NonPixelShader);
			},
			[=](FFXCACAOPassData const& data, RenderGraphContext& ctx)
			{
				static_assert(sizeof(Matrix) == sizeof(FfxFloat32x4x4));

				GfxTexture& depth_texture = ctx.GetTexture(*data.depth);
				GfxTexture& normal_texture = ctx.GetTexture(*data.gbuffer_normal);
				GfxTexture& output_texture = ctx.GetTexture(*data.output);

				FfxCacaoContext* current_cacao_context = use_downsampled_ssao ? &cacao_downsampled_context : &cacao_context;
				cacao_settings.generateNormals = generate_normals;
				FfxErrorCode error_code = ffxCacaoUpdateSettings(current_cacao_context, &cacao_settings, use_downsampled_ssao);
				ADRIA_ASSERT(error_code == FFX_OK);

				GfxCommandList* cmd_list = ctx.GetCommandList();
				FfxCacaoDispatchDescription cacao_dispatch_desc{};
				cacao_dispatch_desc.commandList = ffxGetCommandListDX12((ID3D12GraphicsCommandList*)cmd_list->GetNative());

				FfxFloat32x4x4 proj, world_to_view;
				Matrix camera_proj(frame_data.camera_proj); 
				camera_proj.Transpose(camera_proj);
				memcpy(&proj, &frame_data.camera_proj, sizeof(FfxFloat32x4x4));
				memcpy(&world_to_view, &Matrix::Identity, sizeof(FfxFloat32x4x4));

				cacao_dispatch_desc.depthBuffer  = GetFfxResource(depth_texture);
				cacao_dispatch_desc.normalBuffer = GetFfxResource(normal_texture);
				cacao_dispatch_desc.outputBuffer = GetFfxResource(output_texture, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
				cacao_dispatch_desc.proj = &proj;
				cacao_dispatch_desc.normalsToView = &world_to_view;
				cacao_dispatch_desc.normalUnpackMul = 2.0f;
				cacao_dispatch_desc.normalUnpackAdd = -1.0f;
				error_code = ffxCacaoContextDispatch(current_cacao_context, &cacao_dispatch_desc);
				ADRIA_ASSERT(error_code == FFX_OK);

				cmd_list->ResetState();
			}, RGPassType::Compute);

		cacao_settings = FfxCacaoPresets[preset_id].cacao_settings;
		use_downsampled_ssao = FfxCacaoPresets[preset_id].use_downsampled_ssao;
	}

	void FFXCACAOPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx(name_version, ImGuiTreeNodeFlags_None))
				{
					ImGui::Combo("Preset", &preset_id,
						[](void* vec, Int idx, const Char** out_text)
						{
							std::vector<std::string>* vector = reinterpret_cast<std::vector<std::string>*>(vec);
							if (idx < 0 || idx >= vector->size()) return false;
							*out_text = vector->at(idx).c_str();
							return true;
						}, reinterpret_cast<void*>(&FfxCacaoPresetNames), (Int32)FfxCacaoPresetNames.size());

					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_AO);
	}

	void FFXCACAOPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
		if (IsSupported())
		{
			DestroyContext();
			CreateContext();
		}
	}

	void FFXCACAOPass::CreateContext()
	{
		cacao_context_desc.width = width;
		cacao_context_desc.height = height;
		cacao_context_desc.useDownsampledSsao = false;
		cacao_context_desc.backendInterface.device = gfx->GetNative();
		FfxErrorCode error_code = ffxCacaoContextCreate(&cacao_context, &cacao_context_desc);
		ADRIA_ASSERT(error_code == FFX_OK);
		cacao_context_desc.useDownsampledSsao = true;
		error_code = ffxCacaoContextCreate(&cacao_downsampled_context, &cacao_context_desc);
		ADRIA_ASSERT(error_code == FFX_OK);
	}

	void FFXCACAOPass::DestroyContext()
	{
		gfx->WaitForGPU();
		ffxCacaoContextDestroy(&cacao_context);
		ffxCacaoContextDestroy(&cacao_downsampled_context);
	}

}

