#pragma once
#include "RenderGraphContext.h"
#include "Utilities/EnumUtil.h"

namespace adria
{
	enum class RGPassType : Uint8
	{
		Graphics,
		Compute,
		AsyncCompute,
		Copy
	};

	enum class RGPassFlags : Uint32
	{
		None = 0x00,
		ForceNoCull = 0x01,						//RGPass will not be culled by Render Graph, useful for debug passes
		LegacyRenderPass = 0x02,				//RGPass will not use DX12 Render Passes but rather OMSetRenderTargets
	};
	ENABLE_ENUM_BIT_OPERATORS(RGPassFlags);

	enum RGReadAccess : Uint8
	{
		ReadAccess_PixelShader,
		ReadAccess_NonPixelShader,
		ReadAccess_AllShader
	};

	enum class RGLoadAccessOp : Uint8
	{
		Discard,
		Preserve,
		Clear,
		NoAccess
	};

	enum class RGStoreAccessOp : Uint8
	{
		Discard,
		Preserve,
		Resolve,
		NoAccess
	};

	inline constexpr Uint8 CombineAccessOps(RGLoadAccessOp load_op, RGStoreAccessOp store_op)
	{
		return (Uint8)load_op << 2 | (Uint8)store_op;
	}
	enum class RGLoadStoreAccessOp : Uint8
	{
		Discard_Discard = CombineAccessOps(RGLoadAccessOp::Discard, RGStoreAccessOp::Discard),
		Discard_Preserve = CombineAccessOps(RGLoadAccessOp::Discard, RGStoreAccessOp::Preserve),
		Clear_Preserve = CombineAccessOps(RGLoadAccessOp::Clear, RGStoreAccessOp::Preserve),
		Preserve_Preserve = CombineAccessOps(RGLoadAccessOp::Preserve, RGStoreAccessOp::Preserve),
		Clear_Discard = CombineAccessOps(RGLoadAccessOp::Clear, RGStoreAccessOp::Discard),
		Preserve_Discard = CombineAccessOps(RGLoadAccessOp::Preserve, RGStoreAccessOp::Discard),
		Clear_Resolve = CombineAccessOps(RGLoadAccessOp::Clear, RGStoreAccessOp::Resolve),
		Preserve_Resolve = CombineAccessOps(RGLoadAccessOp::Preserve, RGStoreAccessOp::Resolve),
		Discard_Resolve = CombineAccessOps(RGLoadAccessOp::Discard, RGStoreAccessOp::Resolve),
		NoAccess_NoAccess = CombineAccessOps(RGLoadAccessOp::NoAccess, RGStoreAccessOp::NoAccess),
	};

	inline constexpr void SplitAccessOp(RGLoadStoreAccessOp load_store_op, RGLoadAccessOp& load_op, RGStoreAccessOp& store_op)
	{
		store_op = static_cast<RGStoreAccessOp>((Uint8)load_store_op & 0b11);
		load_op = static_cast<RGLoadAccessOp>(((Uint8)load_store_op >> 2) & 0b11);
	}

	class RenderGraph;
	class RenderGraphBuilder;
	class GfxDevice;

	class RenderGraphPassBase
	{
		friend RenderGraph;
		friend RenderGraphBuilder;

		struct RenderTargetInfo
		{
			RGRenderTargetId render_target_handle;
			RGLoadStoreAccessOp render_target_access;
		};
		struct DepthStencilInfo
		{
			RGDepthStencilId depth_stencil_handle;
			RGLoadStoreAccessOp depth_access;
			RGLoadStoreAccessOp stencil_access;
			Bool depth_read_only;
		};

		inline static Uint32 unique_pass_id = 0;

	public:
		explicit RenderGraphPassBase(Char const* name, RGPassType type = RGPassType::Graphics, RGPassFlags flags = RGPassFlags::None)
			: name(name), type(type), flags(flags), id(0) 
		{
		}
		virtual ~RenderGraphPassBase() = default;

	protected:

		virtual void Setup(RenderGraphBuilder&) = 0;
		virtual void Execute(RenderGraphContext&, GfxCommandList*) const = 0;

		Bool IsCulled() const { return CanBeCulled() && ref_count == 0; }
		Bool CanBeCulled() const { return !HasFlag(flags, RGPassFlags::ForceNoCull); }
		Bool UseLegacyRenderPasses() const { return HasFlag(flags, RGPassFlags::LegacyRenderPass); }

	private:
		std::string const name;
		Uint64 ref_count = 0ull;
		RGPassType type;
		RGPassFlags flags = RGPassFlags::None;
		Uint64 id;

		std::unordered_set<RGTextureId> texture_creates;
		std::unordered_set<RGTextureId> texture_reads;
		std::unordered_set<RGTextureId> texture_writes;
		std::unordered_set<RGTextureId> texture_destroys;
		std::unordered_map<RGTextureId, GfxResourceState> texture_state_map;
		
		std::unordered_set<RGBufferId> buffer_creates;
		std::unordered_set<RGBufferId> buffer_reads;
		std::unordered_set<RGBufferId> buffer_writes;
		std::unordered_set<RGBufferId> buffer_destroys;
		std::unordered_map<RGBufferId, GfxResourceState> buffer_state_map;

		std::vector<RenderTargetInfo> render_targets_info;
		std::optional<DepthStencilInfo> depth_stencil = std::nullopt;
		Uint32 viewport_width = 0, viewport_height = 0;

		std::vector<Uint32>				events_to_start;
		Uint32							num_events_to_end = 0;

		Uint64 wait_graphics_pass_id	= UINT64_MAX;
		Uint64 signal_graphics_pass_id	= UINT64_MAX;
		Uint64 signal_value				= UINT64_MAX;
		Uint64 wait_value				= UINT64_MAX;
	};
	using RGPassBase = RenderGraphPassBase;

	template<typename PassData>
	class RenderGraphPass final : public RenderGraphPassBase
	{
	public:
		using SetupFunc = std::function<void(PassData&, RenderGraphBuilder&)>;
		using ExecuteFunc = std::function<void(PassData const&, RenderGraphContext&, GfxCommandList*)>;

	public:
		RenderGraphPass(Char const* name, SetupFunc&& setup, ExecuteFunc&& execute, RGPassType type = RGPassType::Graphics, RGPassFlags flags = RGPassFlags::None)
			: RenderGraphPassBase(name, type, flags), setup(std::move(setup)), execute(std::move(execute))
		{}

		PassData const& GetPassData() const
		{
			return data;
		}

	private:
		PassData data;
		SetupFunc setup;
		ExecuteFunc execute;

	private:

		void Setup(RenderGraphBuilder& builder) override
		{
			ADRIA_ASSERT(setup != nullptr && "setup function is null!");
			setup(data, builder);
		}

		void Execute(RenderGraphContext& context, GfxCommandList* ctx) const override
		{
			ADRIA_ASSERT(setup != nullptr && "execute function is null!");
			execute(data, context, ctx);
		}
	};

	template<>
	class RenderGraphPass<void> final : public RenderGraphPassBase
	{
	public:
		using SetupFunc = std::function<void(RenderGraphBuilder&)>;
		using ExecuteFunc = std::function<void(RenderGraphContext&, GfxCommandList*)>;

	public:
		RenderGraphPass(Char const* name, SetupFunc&& setup, ExecuteFunc&& execute, RGPassType type = RGPassType::Graphics, RGPassFlags flags = RGPassFlags::None)
			: RenderGraphPassBase(name, type, flags), setup(std::move(setup)), execute(std::move(execute))
		{}

		void GetPassData() const
		{
			return;
		}

	private:
		SetupFunc setup;
		ExecuteFunc execute;

	private:

		void Setup(RenderGraphBuilder& builder) override
		{
			ADRIA_ASSERT(setup != nullptr && "setup function is null!");
			setup(builder);
		}

		void Execute(RenderGraphContext& context, GfxCommandList* ctx) const override
		{
			ADRIA_ASSERT(setup != nullptr && "execute function is null!");
			execute(context, ctx);
		}
	};

	template<typename PassData>
	using RGPass = RenderGraphPass<PassData>;

	inline std::string RGPassTypeToString(RGPassType type)
	{
		switch (type)
		{
		case RGPassType::Graphics: return "Graphics";
		case RGPassType::Compute: return "Compute";
		case RGPassType::AsyncCompute: return "ComputeAsync";
		case RGPassType::Copy: return "Copy";
		}
		return "Invalid";
	}
};