#include "PickingPass.h"
#include "BlackboardData.h"
#include "ShaderManager.h" 
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxBuffer.h"
#include "Graphics/GfxBufferView.h"
#include "Graphics/GfxPipelineState.h"
#include "RenderGraph/RenderGraph.h"

namespace adria
{

	PickingPass::PickingPass(GfxDevice* gfx, Uint32 width, Uint32 height) : gfx(gfx), width(width), height(height)
	{
		CreatePSO();
		CreatePickingBuffers();
	}

	void PickingPass::OnResize(Uint32 w, Uint32 h)
	{
		width = w, height = h;
	}

	void PickingPass::AddPass(RenderGraph& rg)
	{
		RG_SCOPE(rg, "Picking");

		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct PickingPassDispatchData
		{
			RGBufferReadWriteId pick_buffer;
			RGTextureReadOnlyId depth;
			RGTextureReadOnlyId normal;
		};

		rg.AddPass<PickingPassDispatchData>("Picking Pass Dispatch",
			[=](PickingPassDispatchData& data, RenderGraphBuilder& builder)
			{
				RGBufferDesc pick_buffer_desc{};
				pick_buffer_desc.resource_usage = GfxResourceUsage::Default;
				pick_buffer_desc.misc_flags = GfxBufferMiscFlag::BufferStructured;
				pick_buffer_desc.stride = sizeof(PickingData);
				pick_buffer_desc.size = pick_buffer_desc.stride;
				builder.DeclareBuffer(RG_NAME(PickBuffer), pick_buffer_desc);

				data.pick_buffer = builder.WriteBuffer(RG_NAME(PickBuffer));
				data.depth = builder.ReadTexture(RG_NAME(DepthStencil), ReadAccess_NonPixelShader);
				data.normal = builder.ReadTexture(RG_NAME(GBufferNormal), ReadAccess_NonPixelShader);
			},
			[=](PickingPassDispatchData const& data, RenderGraphContext& context)
			{
				GfxDevice* gfx = context.GetDevice();
				GfxCommandList* cmd_list = context.GetCommandList();
				
				GfxDescriptor src_descriptors[] =
				{
					context.GetReadOnlyTexture(data.depth),
					context.GetReadOnlyTexture(data.normal),
					context.GetReadWriteBuffer(data.pick_buffer)
				};	
				GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(src_descriptors);

				struct PickingConstants
				{
					Uint32 depth_idx;
					Uint32 normal_idx;
					Uint32 buffer_idx;
				} constants =
				{
					.depth_idx = table, .normal_idx = table + 1, .buffer_idx = table + 2
				};
				
				cmd_list->SetPipelineState(picking_pso->Get());
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(width, 16), DivideAndRoundUp(height, 16), 1);
				cmd_list->Dispatch(1, 1, 1);
			}, RGPassType::Compute, RGPassFlags::ForceNoCull);

		struct PickingPassCopyData
		{
			RGBufferCopySrcId src;
		};

		rg.AddPass<PickingPassCopyData>("Picking Pass Copy",
			[=](PickingPassCopyData& data, RenderGraphBuilder& builder)
			{
				data.src = builder.ReadCopySrcBuffer(RG_NAME(PickBuffer));
			},
			[=, backbuffer_index = gfx->GetBackbufferIndex()](PickingPassCopyData const& data, RenderGraphContext& ctx)
			{
				GfxCommandList* cmd_list = ctx.GetCommandList();
				GfxBuffer const& buffer = ctx.GetCopySrcBuffer(data.src);
				cmd_list->CopyBuffer(*read_picking_buffers[backbuffer_index], buffer);
			}, RGPassType::Copy, RGPassFlags::ForceNoCull);
	}

	PickingData PickingPass::GetPickingData() const
	{
		Uint32 backbuffer_index = gfx->GetBackbufferIndex();
		PickingData const* data = read_picking_buffers[backbuffer_index]->GetMappedData<PickingData>();
		PickingData picking_data = *data;
		return picking_data;
	}

	void PickingPass::CreatePSO()
	{
		GfxComputePipelineStateDesc compute_pso_desc{};
		compute_pso_desc.CS = CS_Picking;
		picking_pso = gfx->CreateManagedComputePipelineState(compute_pso_desc);
	}

	void PickingPass::CreatePickingBuffers()
	{
		for (Uint64 i = 0; i < gfx->GetBackbufferCount(); ++i)
		{
			read_picking_buffers.emplace_back(gfx->CreateBuffer(ReadBackBufferDesc(sizeof(PickingData))));
		}
	}

}

