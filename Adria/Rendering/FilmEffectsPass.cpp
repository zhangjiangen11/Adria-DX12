#include "FilmEffectsPass.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "PostProcessor.h" 
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"
#include "Core/ConsoleManager.h"

namespace adria
{
	static TAutoConsoleVariable<Bool> FilmEffects("r.FilmEffects", false, "Enable or Disable Film Effects");

	FilmEffectsPass::FilmEffectsPass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h)
	{
		CreatePSO();
	}

	void FilmEffectsPass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct FilmEffectsPassData
		{
			RGTextureReadOnlyId  input;
			RGTextureReadWriteId output;
		};

		rg.AddPass<FilmEffectsPassData>("Film Effects Pass",
			[=](FilmEffectsPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc film_effects_desc{};
				film_effects_desc.format = GfxFormat::R16G16B16A16_FLOAT;
				film_effects_desc.width = width;
				film_effects_desc.height = height;

				builder.DeclareTexture(RG_NAME(FilmEffectsOutput), film_effects_desc);
				data.output = builder.WriteTexture(RG_NAME(FilmEffectsOutput));
				data.input = builder.ReadTexture(postprocessor->GetFinalResource());
			},
			[=](FilmEffectsPassData const& data, RenderGraphContext& ctx)
			{
				GfxDevice* gfx = ctx.GetDevice();
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxDescriptor src_descriptors[] =
				{
					ctx.GetReadOnlyTexture(data.input),
					ctx.GetReadWriteTexture(data.output)
				};
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_descriptors);

				struct FilmEffectsConstants
				{
					Bool32  lens_distortion_enabled;
					Float	lens_distortion_intensity;
					Bool32  chromatic_aberration_enabled;
					Float   chromatic_aberration_intensity;
					Bool32  vignette_enabled;
					Float   vignette_intensity;
					Bool32  film_grain_enabled;
					Float   film_grain_scale;
					Float   film_grain_amount;
					Uint32  film_grain_seed;
					Uint32  input_idx;
					Uint32  output_idx;
				} constants =
				{
					.lens_distortion_enabled = lens_distortion_enabled,
					.lens_distortion_intensity = lens_distortion_intensity,
					.chromatic_aberration_enabled = chromatic_aberration_enabled,
					.chromatic_aberration_intensity = chromatic_aberration_intensity,
					.vignette_enabled = vignette_enabled,
					.vignette_intensity = vignette_intensity,
					.film_grain_enabled = film_grain_enabled,
					.film_grain_scale = film_grain_scale,
					.film_grain_amount = film_grain_amount,
					.film_grain_seed = GetFilmGrainSeed(frame_data.delta_time, film_grain_seed_update_rate),
					.input_idx = table + 0,
					.output_idx = table + 1
				};
				cmd_list->SetPipelineState(film_effects_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootCBV(2, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
			}, RGPassType::Compute, RGPassFlags::None);

		postprocessor->SetFinalResource(RG_NAME(FilmEffectsOutput));
	}

	void FilmEffectsPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	Bool FilmEffectsPass::IsEnabled(PostProcessor const*) const
	{
		return FilmEffects.Get();
	}

	void FilmEffectsPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("Film Effects", ImGuiTreeNodeFlags_None))
				{
					ImGui::Checkbox("Enable Film Effects", FilmEffects.GetPtr());
					if (FilmEffects.Get())
					{
						ImGui::Checkbox("Lens Distortion", &lens_distortion_enabled);
						ImGui::Checkbox("Chromatic Aberration", &chromatic_aberration_enabled);
						ImGui::Checkbox("Vignette", &vignette_enabled);
						ImGui::Checkbox("Film Grain", &film_grain_enabled);
						if (lens_distortion_enabled)
						{
							ImGui::SliderFloat("Lens Distortion Intensity", &lens_distortion_intensity, -1.0f, 1.0f);
						}
						if (chromatic_aberration_enabled)
						{
							ImGui::SliderFloat("Chromatic Aberration Intensity", &chromatic_aberration_intensity, 0.0f, 40.0f);
						}
						if (vignette_enabled)
						{
							ImGui::SliderFloat("Vignette Intensity", &vignette_intensity, 0.0f, 2.0f);
						}
						if (film_grain_enabled)
						{
							ImGui::SliderFloat("Film Grain Scale", &film_grain_scale, 0.01f, 20.0f);
							ImGui::SliderFloat("Film Grain Amount", &film_grain_amount, 0.0f, 20.0f);
							ImGui::SliderFloat("Film Grain Seed Update Rate", &film_grain_seed_update_rate, 0.0f, 0.1f);
						}
					}
					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing
		);
	}

	void FilmEffectsPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_FilmEffects;
		film_effects_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

	Uint32 FilmEffectsPass::GetFilmGrainSeed(Float dt, Float seed_update_rate)
	{
		static Uint32 seed_counter = 0;
		static Float time_counter = 0.0;
		time_counter += dt;
		if (time_counter >= seed_update_rate)
		{
			++seed_counter;
			time_counter = 0.0;
		}
		return seed_counter;
	}

}

