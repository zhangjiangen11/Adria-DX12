#pragma once
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxCapabilities.h"

namespace adria
{
    struct MetalCapabilities : GfxCapabilities
    {
        virtual Bool Initialize(GfxDevice* gfx) override {return true;}
    };

    class MetalDevice : public GfxDevice
    {
    public:
        explicit MetalDevice(Window* window);
        ~MetalDevice() override;

        // Pure virtual methods from GfxDevice that must be implemented
        void OnResize(Uint32 w, Uint32 h) override;
        GfxTexture* GetBackbuffer() const override;
        Uint32 GetBackbufferIndex() const override;
        Uint32 GetFrameIndex() const override;
        Uint32 GetBackbufferCount() const override { return 2; }

        void SetRenderingNotStarted() override {}
        void InitGlobalResourceBindings(Uint32 max_resources) override {}

        void Update() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        Bool IsFirstFrame() override { return false; }

        void* GetNative() const override { return nullptr; }
        void* GetWindowHandle() const override { return nullptr; }

        GfxCapabilities const& GetCapabilities() const override;
        GfxVendor GetVendor() const override { return GfxVendor::Apple; }
        GfxBackend GetBackend() const override { return GfxBackend::Metal; }

        GfxNsightPerfManager* GetNsightPerfManager() const override { return nullptr; }

        void WaitForGPU() override {}
        GfxCommandQueue* GetCommandQueue(GfxCommandListType type) const override { return nullptr; }
        GfxFence& GetFence(GfxCommandListType type) override;
        Uint64 GetFenceValue(GfxCommandListType type) const override { return 0; }
        void SetFenceValue(GfxCommandListType type, Uint64 value) override {}

        GfxCommandList* GetCommandList(GfxCommandListType type) const override { return nullptr; }
        GfxCommandList* GetLatestCommandList(GfxCommandListType type) const override { return nullptr; }
        GfxCommandList* AllocateCommandList(GfxCommandListType type) const override { return nullptr; }
        void FreeCommandList(GfxCommandList*, GfxCommandListType type) override {}

        GfxLinearDynamicAllocator* GetDynamicAllocator() const override { return nullptr; }

        GfxBindlessTable AllocatePersistentBindlessTable(Uint32 count, GfxDescriptorType type = GfxDescriptorType::CBV_SRV_UAV) override { return {}; }
        GfxBindlessTable AllocateBindlessTable(Uint32 count, GfxDescriptorType type = GfxDescriptorType::CBV_SRV_UAV) override { return {}; }
        void UpdateBindlessTable(GfxBindlessTable table, std::span<GfxDescriptor const> src_descriptors) override {}
        void UpdateBindlessTable(GfxBindlessTable table, Uint32 table_offset, GfxDescriptor src_descriptor, Uint32 src_count = 1) override {}
        void UpdateBindlessTables(std::vector<GfxBindlessTable> const& table, std::span<std::pair<GfxDescriptor, Uint32>> src_range_starts_and_size) override {}
        void FreeDescriptor(GfxDescriptor descriptor) override {}

        std::unique_ptr<GfxCommandList> CreateCommandList(GfxCommandListType type) override { return nullptr; }
        std::unique_ptr<GfxTexture> CreateTexture(GfxTextureDesc const& desc) override { return nullptr; }
        std::unique_ptr<GfxTexture> CreateTexture(GfxTextureDesc const& desc, GfxTextureData const& data) override { return nullptr; }
        std::unique_ptr<GfxTexture> CreateBackbufferTexture(GfxTextureDesc const& desc, void* backbuffer) override { return nullptr; }
        std::unique_ptr<GfxBuffer> CreateBuffer(GfxBufferDesc const& desc, GfxBufferData const& initial_data) override { return nullptr; }
        std::unique_ptr<GfxBuffer> CreateBuffer(GfxBufferDesc const& desc) override { return nullptr; }

        std::shared_ptr<GfxBuffer> CreateBufferShared(GfxBufferDesc const& desc, GfxBufferData const& initial_data) override { return nullptr; }
        std::shared_ptr<GfxBuffer> CreateBufferShared(GfxBufferDesc const& desc) override { return nullptr; }

        std::unique_ptr<GfxPipelineState> CreateGraphicsPipelineState(GfxGraphicsPipelineStateDesc const& desc) override { return nullptr; }
        std::unique_ptr<GfxPipelineState> CreateComputePipelineState(GfxComputePipelineStateDesc const& desc) override { return nullptr; }
        std::unique_ptr<GfxPipelineState> CreateMeshShaderPipelineState(GfxMeshShaderPipelineStateDesc const& desc) override { return nullptr; }
        std::unique_ptr<GfxFence> CreateFence(Char const* name) override { return nullptr; }
        std::unique_ptr<GfxQueryHeap> CreateQueryHeap(GfxQueryHeapDesc const& desc) override { return nullptr; }
        std::unique_ptr<GfxRayTracingTLAS> CreateRayTracingTLAS(std::span<GfxRayTracingInstance> instances, GfxRayTracingASFlags flags) override { return nullptr; }
        std::unique_ptr<GfxRayTracingBLAS> CreateRayTracingBLAS(std::span<GfxRayTracingGeometry> geometries, GfxRayTracingASFlags flags) override { return nullptr; }
        std::unique_ptr<GfxRayTracingPipeline> CreateRayTracingPipeline(GfxRayTracingPipelineDesc const& desc) override { return nullptr; }

        GfxDescriptor CreateBufferSRV(GfxBuffer const*, GfxBufferDescriptorDesc const* = nullptr) override { return {}; }
        GfxDescriptor CreateBufferUAV(GfxBuffer const*, GfxBufferDescriptorDesc const* = nullptr) override { return {}; }
        GfxDescriptor CreateBufferUAV(GfxBuffer const*, GfxBuffer const*, GfxBufferDescriptorDesc const* = nullptr) override { return {}; }
        GfxDescriptor CreateTextureSRV(GfxTexture const*, GfxTextureDescriptorDesc const* = nullptr) override { return {}; }
        GfxDescriptor CreateTextureUAV(GfxTexture const*, GfxTextureDescriptorDesc const* = nullptr) override { return {}; }
        GfxDescriptor CreateTextureRTV(GfxTexture const*, GfxTextureDescriptorDesc const* = nullptr) override { return {}; }
        GfxDescriptor CreateTextureDSV(GfxTexture const*, GfxTextureDescriptorDesc const* = nullptr) override { return {}; }

        Uint64 GetLinearBufferSize(GfxTexture const* texture) const override { return 0; }
        Uint64 GetLinearBufferSize(GfxBuffer const* buffer) const override { return 0; }

        GfxShadingRateInfo const& GetShadingRateInfo() const override;
        void SetShadingRateInfo(GfxShadingRateInfo const& info) override {}

        void GetTimestampFrequency(Uint64& frequency) const override { frequency = 0; }
        GPUMemoryUsage GetMemoryUsage() const override { return {0, 0}; }

    private:
        void AddToReleaseQueue_Internal(ReleasableObject* _obj) override {}

        Window* window = nullptr;
        Uint32 frame_index = 0;
        MetalCapabilities capabilities;
        GfxShadingRateInfo shading_rate_info;
        GfxFence* dummy_fence = nullptr;
    };
}
