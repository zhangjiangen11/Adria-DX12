#include "D3D12_FFXCASPass.h"
#include "FidelityFXUtils.h"
#include "BlackboardData.h"
#include "Postprocessor.h"
#include "Graphics/GfxDevice.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"
#include "Core/ConsoleManager.h"

namespace adria
{
	static TAutoConsoleVariable<Bool> CAS("r.CAS", false, "Enable or Disable Contrast-Adaptive Sharpening, TAA must be enabled");

	D3D12_FFXCASPass::D3D12_FFXCASPass(GfxDevice* gfx, Uint32 w, Uint32 h) : gfx(gfx), width(w), height(h), ffx_interface(nullptr)
	{
		if (!gfx->GetCapabilities().SupportsShaderModel(SM_6_6)) return;
		sprintf(name_version, "FFX CAS %d.%d.%d", FFX_CAS_VERSION_MAJOR, FFX_CAS_VERSION_MINOR, FFX_CAS_VERSION_PATCH);
		ffx_interface = CreateFfxInterface(gfx, FFX_CAS_CONTEXT_COUNT);
		cas_context_desc.backendInterface = *ffx_interface;
		CreateContext();
	}

	D3D12_FFXCASPass::~D3D12_FFXCASPass()
	{
		DestroyContext();
		DestroyFfxInterface(ffx_interface);
	}

	void D3D12_FFXCASPass::AddPass(RenderGraph& rg, PostProcessor* postprocessor)
	{
		struct FFXCASPassData
		{
			RGTextureReadOnlyId input;
			RGTextureReadWriteId output;
		};

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		rg.AddPass<FFXCASPassData>(name_version,
			[=](FFXCASPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc ffx_dof_desc = builder.GetTextureDesc(postprocessor->GetFinalResource());
				builder.DeclareTexture(RG_NAME(FFXCASOutput), ffx_dof_desc);

				data.output = builder.WriteTexture(RG_NAME(FFXCASOutput));
				data.input = builder.ReadTexture(postprocessor->GetFinalResource(), ReadAccess_NonPixelShader);
			},
			[=](FFXCASPassData const& data, RenderGraphContext& ctx)
			{
				GfxCommandList* cmd_list = ctx.GetCommandList();

				GfxTexture& input_texture = ctx.GetTexture(*data.input);
				GfxTexture& output_texture = ctx.GetTexture(*data.output);

				FfxCasDispatchDescription cas_dispatch_desc{};
				cas_dispatch_desc.commandList = ffxGetCommandListDX12((ID3D12CommandList*)cmd_list->GetNative());
				cas_dispatch_desc.color = GetFfxResource(input_texture);
				cas_dispatch_desc.output = GetFfxResource(output_texture, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
				cas_dispatch_desc.renderSize = { input_texture.GetWidth(), input_texture.GetHeight() };
				cas_dispatch_desc.sharpness = sharpness;

				FfxErrorCode errorCode = ffxCasContextDispatch(&cas_context, &cas_dispatch_desc);
				ADRIA_ASSERT(errorCode == FFX_OK);

				cmd_list->ResetState();
			}, RGPassType::Compute);
		postprocessor->SetFinalResource(RG_NAME(FFXCASOutput));
	}

	void D3D12_FFXCASPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
		DestroyContext();
		CreateContext();
	}

	Bool D3D12_FFXCASPass::IsEnabled(PostProcessor const*) const
	{
		return CAS.Get();
	}

	void D3D12_FFXCASPass::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx(name_version, ImGuiTreeNodeFlags_None))
				{
					ImGui::Checkbox("Enable", CAS.GetPtr());
					if (CAS.Get())
					{
						ImGui::SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f, "%.2f");
					}
					ImGui::TreePop();
				}
			}, GUICommandGroup_PostProcessing, GUICommandSubGroup_Antialiasing);
	}

	Bool D3D12_FFXCASPass::IsGUIVisible(PostProcessor const* postprocessor) const
	{
		return postprocessor->HasTAA() || postprocessor->HasUpscaler();
	}

	void D3D12_FFXCASPass::CreateContext()
	{
		cas_context_desc.colorSpaceConversion = FFX_CAS_COLOR_SPACE_LINEAR;
		cas_context_desc.flags |= FFX_CAS_SHARPEN_ONLY;
		cas_context_desc.maxRenderSize.width = width;
		cas_context_desc.maxRenderSize.height = height;
		cas_context_desc.displaySize.width = width;
		cas_context_desc.displaySize.height = height;
		FfxErrorCode error_code = ffxCasContextCreate(&cas_context, &cas_context_desc);
		ADRIA_ASSERT(error_code == FFX_OK);
	}

	void D3D12_FFXCASPass::DestroyContext()
	{
		gfx->WaitForGPU();
		ffxCasContextDestroy(&cas_context);
	}

}


