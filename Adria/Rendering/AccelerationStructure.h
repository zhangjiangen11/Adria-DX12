#pragma once
#include "Graphics/GfxFence.h"
#include "Graphics/GfxDescriptor.h"
#include "Graphics/GfxRayTracingAS.h"

namespace adria
{
	class GfxDevice;
	class GfxBuffer;
	struct Mesh;

	class AccelerationStructure
	{
	public:
		explicit AccelerationStructure(GfxDevice* gfx);

		void AddInstance(Mesh const& mesh);
		void Build();
		void Clear();

		Int32 GetTLASIndex() const;

	private:
		GfxDevice* gfx;
		std::vector<GfxRayTracingGeometry> rt_geometries;
		std::vector<std::unique_ptr<GfxRayTracingBLAS>> blases;

		std::vector<GfxRayTracingInstance> rt_instances;
		std::unique_ptr<GfxRayTracingTLAS> tlas;
		GfxDescriptor tlas_srv;

		std::unique_ptr<GfxFence> build_fence;
		Uint64 build_fence_value = 0;

	private:
		void BuildBottomLevels();
		void BuildTopLevel();
	};
}