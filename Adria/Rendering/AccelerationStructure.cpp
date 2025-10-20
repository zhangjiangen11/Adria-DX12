#include "AccelerationStructure.h"
#include "Components.h"
#include "Graphics/GfxBuffer.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxCommandList.h"

namespace adria
{

	AccelerationStructure::AccelerationStructure(GfxDevice* gfx) : gfx(gfx)
	{
		build_fence = gfx->CreateFence("Build Fence");
		++build_fence_value;
	}

	void AccelerationStructure::AddInstance(Mesh const& mesh)
	{
		GfxLinearDynamicAllocator* dynamic_allocator = gfx->GetDynamicAllocator();
		blases.resize(mesh.instances.size());
		Uint32 instance_id = 0;

		GfxBuffer* geometry_buffer = g_GeometryBufferCache.GetGeometryBuffer(mesh.geometry_buffer_handle);
		for (SubMeshInstance const& instance : mesh.instances)
		{
			SubMeshGPU const& submesh = mesh.submeshes[instance.submesh_index];
			Material const& material = mesh.materials[submesh.material_index];

			GfxRayTracingGeometry& rt_geometry = rt_geometries.emplace_back();
			rt_geometry.vertex_buffer = geometry_buffer;
			rt_geometry.vertex_buffer_offset = submesh.positions_offset;
			rt_geometry.vertex_format = GfxFormat::R32G32B32_FLOAT;
			rt_geometry.vertex_stride = GetGfxFormatStride(rt_geometry.vertex_format);
			rt_geometry.vertex_count = submesh.vertices_count;

			rt_geometry.index_buffer = geometry_buffer;
			rt_geometry.index_buffer_offset = submesh.indices_offset;
			rt_geometry.index_count = submesh.indices_count;
			rt_geometry.index_format = GfxFormat::R32_UINT;
			rt_geometry.opaque = material.alpha_mode == MaterialAlphaMode::Opaque;

			GfxRayTracingInstance& rt_instance = rt_instances.emplace_back();
			rt_instance.flags = GfxRayTracingInstanceFlag_None;
			rt_instance.instance_id = instance_id++;
			rt_instance.instance_mask = 0xff;
			auto const T = XMMatrixTranspose(instance.world_transform);
			memcpy(rt_instance.transform, &T, sizeof(T));
		}
	}

	void AccelerationStructure::Build()
	{
		if (blases.empty())
		{
			return;
		}

		BuildBottomLevels();
		for (GfxRayTracingInstance& rt_instance : rt_instances)
		{
			rt_instance.blas = blases[rt_instance.instance_id].get();
		}
		BuildTopLevel();
		tlas_srv = gfx->CreateBufferSRV(&tlas->GetBuffer());
	}

	void AccelerationStructure::Clear()
	{
		blases.clear();
		rt_geometries.clear();
		rt_instances.clear();
		tlas = nullptr;
	}

	Int32 AccelerationStructure::GetTLASIndex() const
	{
		GfxBindlessTable table = gfx->AllocateBindlessTable(1);
		gfx->UpdateBindlessTable(table, 0, tlas_srv);
		Int32 new_index = (Int32)table.base;
		return new_index;
	}

	void AccelerationStructure::BuildBottomLevels()
	{
		GfxCommandList* cmd_list = gfx->GetGraphicsCommandList();

		std::span<GfxRayTracingGeometry> geometry_span(rt_geometries);
		for (Uint64 i = 0; i < blases.size(); ++i)
		{
			blases[i] = gfx->CreateRayTracingBLAS(geometry_span.subspan(i, 1), GfxRayTracingASFlag_PreferFastTrace);
		}
		cmd_list->Signal(*build_fence, build_fence_value);
		cmd_list->End();
		cmd_list->Submit();
	}

	void AccelerationStructure::BuildTopLevel()
	{
		GfxCommandList* cmd_list = gfx->GetGraphicsCommandList();
		cmd_list->Begin();

		build_fence->Wait(build_fence_value);
		++build_fence_value;

		tlas = gfx->CreateRayTracingTLAS(rt_instances, GfxRayTracingASFlag_PreferFastTrace);

		cmd_list->Signal(*build_fence, build_fence_value);
		cmd_list->End();
		cmd_list->Submit();

		build_fence->Wait(build_fence_value);
		++build_fence_value;

		cmd_list->Begin();
	}
}

