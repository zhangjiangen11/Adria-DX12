#include "AutoExposurePass.h"
#include "BlackboardData.h"
#include "ShaderManager.h"
#include "Postprocessor.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/GfxTexture.h"
#include "Graphics/GfxBuffer.h"
#include "Graphics/GfxPipelineState.h"
#include "Editor/GUICommand.h"
#include "Core/ConsoleManager.h"

namespace adria
{
	static TAutoConsoleVariable<Bool>  AutoExposure("r.AutoExposure", true, "Enable or Disable Auto Exposure");
	static TAutoConsoleVariable<Float> MinLogLuminance("r.AutoExposure.MinLogLuminance", -5.0f, "Min Log Luminance for Auto Exposure");
	static TAutoConsoleVariable<Float> MaxLogLuminance("r.AutoExposure.MaxLogLuminance", 20.0f, "Max Log Luminance for Auto Exposure");
	static TAutoConsoleVariable<Float> AdaptionSpeed("r.AutoExposure.AdaptionSpeed", 2.5f, "Adaption Speed for Auto Exposure");

	AutoExposurePass::AutoExposurePass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h)
	{
		CreatePSOs();
	}

	void AutoExposurePass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		struct BuildHistogramData
		{
			RGTextureReadOnlyId scene_texture;
			RGBufferReadWriteId histogram_buffer;
		};
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		rg.AddPass<BuildHistogramData>("Build Histogram Pass",
			[=](BuildHistogramData& data, RenderGraphBuilder& builder)
			{
				data.scene_texture = builder.ReadTexture(postprocessor->GetFinalResource());

				RGBufferDesc desc{};
				desc.stride = sizeof(Uint32);
				desc.size = desc.stride * 256;
				desc.misc_flags = GfxBufferMiscFlag::BufferRaw;
				desc.resource_usage = GfxResourceUsage::Default;
				builder.DeclareBuffer(RG_NAME(HistogramBuffer), desc);
				data.histogram_buffer = builder.WriteBuffer(RG_NAME(HistogramBuffer));
			},
			[=](BuildHistogramData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxBuffer const& histogram_buffer = ctx.GetBuffer(*data.histogram_buffer);
				Uint32 clear_value[4] = { 0, 0, 0, 0 };
				cmd_list->ClearBuffer(histogram_buffer, clear_value);
				cmd_list->BufferBarrier(histogram_buffer, GfxResourceState::ComputeUAV, GfxResourceState::ComputeUAV);

				cmd_list->FlushBarriers();
				cmd_list->SetPipelineState(build_histogram_pso->Get());

				GfxDescriptor src_handles[] = { ctx.GetReadOnlyTexture(data.scene_texture), ctx.GetReadWriteBuffer(data.histogram_buffer) };
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_handles);

				struct BuildHistogramConstants
				{
					Uint32  width;
					Uint32  height;
					Float   rcp_width;
					Float   rcp_height;
					Float   min_log_luminance;
					Float   log_luminance_range_rcp;
					Uint32  scene_idx;
					Uint32  histogram_idx;
				} constants = { .width = width, .height = height,
								.rcp_width = 1.0f / width, .rcp_height = 1.0f / height,
								.min_log_luminance = MinLogLuminance.Get(), .log_luminance_range_rcp = 1.0f/ (MaxLogLuminance.Get() - MinLogLuminance.Get()),
								.scene_idx = table, .histogram_idx = table + 1 };
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
			}, RGPassType::Compute, RGPassFlags::None);

		rg.ImportTexture(RG_NAME(AverageLuminance), luminance_texture.get());

		struct HistogramReductionData
		{
			RGBufferReadOnlyId		histogram_buffer;
			RGTextureReadWriteId	avg_luminance;
			RGTextureReadWriteId	exposure;

			Uint32					pixel_count;
		};
		rg.AddPass<HistogramReductionData>("Histogram Reduction Pass",
			[=](HistogramReductionData& data, RenderGraphBuilder& builder)
			{
				data.histogram_buffer = builder.ReadBuffer(RG_NAME(HistogramBuffer));
				data.avg_luminance = builder.WriteTexture(RG_NAME(AverageLuminance));

				RGTextureDesc desc{};
				desc.width = desc.height = 1;
				desc.format = GfxFormat::R16_FLOAT;
				builder.DeclareTexture(RG_NAME(Exposure), desc);
				data.exposure = builder.WriteTexture(RG_NAME(Exposure));

				RGTextureDesc const& scene_desc = builder.GetTextureDesc(postprocessor->GetFinalResource());
				data.pixel_count = scene_desc.width * scene_desc.height;
			},
			[=](HistogramReductionData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				if (invalid_history)
				{
					Float clear_value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
					cmd_list->ClearTexture(ctx.GetTexture(*data.avg_luminance), clear_value);
					invalid_history = false;
				}

				cmd_list->SetPipelineState(histogram_reduction_pso->Get());

				GfxDescriptor src_handles[] = { ctx.GetReadOnlyBuffer(data.histogram_buffer), ctx.GetReadWriteTexture(data.avg_luminance), ctx.GetReadWriteTexture(data.exposure) };
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_handles);

				struct HistogramReductionConstants
				{
					Float  min_log_luminance;
					Float  log_luminance_range;
					Float  delta_time;
					Float  adaption_speed;
					Uint32 pixel_count;
					Uint32 histogram_idx;
					Uint32 luminance_idx;
					Uint32 exposure_idx;
				} constants = { .min_log_luminance = MinLogLuminance.Get(), .log_luminance_range = MaxLogLuminance.Get() - MinLogLuminance.Get(),
								.delta_time = frame_data.delta_time, .adaption_speed = AdaptionSpeed.Get(), .pixel_count = data.pixel_count,
								.histogram_idx = table, .luminance_idx = table + 1, .exposure_idx = table + 2 };
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(1, 1, 1);
			}, RGPassType::Compute, RGPassFlags::None);

		if (show_histogram)
		{
			rg.ExportBuffer(RG_NAME(HistogramBuffer), histogram_copy.get());
		}
	}

	void AutoExposurePass::OnSceneInitialized()
	{
		GfxTextureDesc desc{};
		desc.width = 1;
		desc.height = 1;
		desc.mip_levels = 1;
		desc.bind_flags = GfxBindFlag::UnorderedAccess;
		desc.misc_flags = GfxTextureMiscFlag::None;
		desc.initial_state = GfxResourceState::ComputeUAV;
		desc.format = GfxFormat::R16_FLOAT;

		luminance_texture = gfx->CreateTexture(desc);

		GfxBufferDesc hist_desc{};
		hist_desc.stride = sizeof(Uint32);
		hist_desc.size = hist_desc.stride * 256;
		hist_desc.misc_flags = GfxBufferMiscFlag::BufferRaw;
		hist_desc.resource_usage = GfxResourceUsage::Readback;
		histogram_copy = gfx->CreateBuffer(hist_desc);
	}

	void AutoExposurePass::GUI()
	{
		
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("Automatic Exposure", 0))
				{
					ImGui::Checkbox("Enable", AutoExposure.GetPtr());
					if (AutoExposure.Get())
					{
						ImGui::DragFloatRange2("Log Luminance", MinLogLuminance.GetPtr(), MaxLogLuminance.GetPtr(), 1.0f, -100, 50);
						ImGui::SliderFloat("Adaption Speed", AdaptionSpeed.GetPtr(), 0.01f, 5.0f);
						ImGui::Checkbox("Histogram", &show_histogram);
						if (show_histogram)
						{
							auto MaxElement = [](Int32* array, Uint64 count)
								{
									Int32 max_element = INT32_MIN;
									for (Uint64 i = 0; i < count; ++i)
									{
										max_element = std::max(array[i], max_element);
									}
									return max_element;
								};

							ADRIA_ASSERT(histogram_copy->IsMapped());
							Uint64 histogram_size = histogram_copy->GetSize() / sizeof(Int32);
							Int32* hist_data = histogram_copy->GetMappedData<Int32>();
							Int32 max_value = MaxElement(hist_data, histogram_size);
							auto converter = [](void* data, Int32 idx)-> Float
								{
									return static_cast<Float>(*(((Int32*)data) + idx));
								};
							ImGui::PlotHistogram("Luminance Histogram", converter, hist_data, (Int32)histogram_size, 0, NULL, 0.0f, (Float)max_value, ImVec2(0, 80));
						}
					}
					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing
		);
	}

	void AutoExposurePass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	Bool AutoExposurePass::IsEnabled(PostProcessor const*) const
	{
		return AutoExposure.Get();
	}

	void AutoExposurePass::CreatePSOs()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_BuildHistogram;
		build_histogram_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);  

		compute_pso_desc.CS = CS_HistogramReduction;
		histogram_reduction_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

}

