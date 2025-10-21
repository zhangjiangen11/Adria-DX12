#include "RenderGraph.h"
#include "Graphics/GfxCommandList.h"
#include "Graphics/GfxRenderPass.h"
#include "Graphics/GfxScopedEvent.h"
#include "Core/Paths.h"
#include "Core/ConsoleManager.h"
#include "Utilities/StringConversions.h"
#include "Utilities/PathHelpers.h"
#include "tracy/Tracy.hpp"

#if GFX_MULTITHREADED
#define RG_MULTITHREADED 1
#else
#define RG_MULTITHREADED 0
#endif

namespace adria
{
	ADRIA_LOG_CHANNEL(RenderGraph);

	extern Bool g_DumpRenderGraph = false;
#if GFX_PROFILING
	static constexpr Bool g_UseDependencyLevels = false;
#else
	static constexpr Bool g_UseDependencyLevels = true;
#endif

	static TAutoConsoleVariable<Bool> RGCullPasses("rg.CullPasses", true, "Determines if the render graph should cull unused passes or not");
	static TAutoConsoleVariable<Bool> RGAsyncCompute("rg.AsyncCompute", false, "Determines if the async compute is enabled or not");

	RGTextureId RenderGraph::DeclareTexture(RGResourceName name, RGTextureDesc const& desc)
	{
		ADRIA_ASSERT_MSG(texture_name_id_map.find(name) == texture_name_id_map.end(), "Texture with that name has already been declared");
		GfxTextureDesc tex_desc{}; InitGfxTextureDesc(desc, tex_desc);
		textures.emplace_back(new RGTexture(textures.size(), tex_desc, name));
		texture_name_id_map[name] = RGTextureId(textures.size() - 1);
		return RGTextureId(textures.size() - 1);
	}

	RGBufferId RenderGraph::DeclareBuffer(RGResourceName name, RGBufferDesc const& desc)
	{
		ADRIA_ASSERT_MSG(buffer_name_id_map.find(name) == buffer_name_id_map.end(), "Buffer with that name has already been declared");
		GfxBufferDesc buf_desc{}; InitGfxBufferDesc(desc, buf_desc);
		buffers.emplace_back(new RGBuffer(buffers.size(), buf_desc, name));
		buffer_name_id_map[name] = RGBufferId(buffers.size() - 1);
		return RGBufferId(buffers.size() - 1);
	}

	Bool RenderGraph::IsTextureDeclared(RGResourceName name)
	{
		return texture_name_id_map.contains(name);
	}

	Bool RenderGraph::IsBufferDeclared(RGResourceName name)
	{
		return buffer_name_id_map.contains(name);
	}

	void RenderGraph::ImportTexture(RGResourceName name, GfxTexture* texture)
	{
		ADRIA_ASSERT(texture);
		textures.emplace_back(new RGTexture(textures.size(), texture, name));
		textures.back()->SetName();
		texture_name_id_map[name] = RGTextureId(textures.size() - 1);
	}

	void RenderGraph::ImportBuffer(RGResourceName name, GfxBuffer* buffer)
	{
		ADRIA_ASSERT(buffer);
		buffers.emplace_back(new RGBuffer(buffers.size(), buffer, name));
		buffers.back()->SetName();
		buffer_name_id_map[name] = RGBufferId(buffers.size() - 1);
	}

	void RenderGraph::ExportTexture(RGResourceName name, GfxTexture* texture)
	{
		ADRIA_ASSERT_MSG(texture, "Cannot export to a null resource");
		AddExportTextureCopyPass(name, texture);
	}

	void RenderGraph::ExportBuffer(RGResourceName name, GfxBuffer* buffer)
	{
		ADRIA_ASSERT_MSG(buffer, "Cannot export to a null resource");
		AddExportBufferCopyPass(name, buffer);
	}

	Bool RenderGraph::IsValidTextureHandle(RGTextureId handle) const
	{
		return handle.IsValid() && handle.id < textures.size();
	}

	Bool RenderGraph::IsValidBufferHandle(RGBufferId handle) const
	{
		return handle.IsValid() && handle.id < buffers.size();
	}

	RenderGraph::~RenderGraph()
	{
		for (auto& [tex_id, view_vector] : texture_view_map)
		{
			for (GfxDescriptor const& view : view_vector)
			{
				gfx->FreeDescriptor(view);
			}
		}

		for (auto& [buf_id, view_vector] : buffer_view_map)
		{
			for (GfxDescriptor const& view : view_vector)
			{
				gfx->FreeDescriptor(view);
			}
		}
	}

	void RenderGraph::Compile()
	{
		ZoneScopedN("RenderGraph::Compile");
		BuildAdjacencyLists();
		TopologicalSort();
		if (g_UseDependencyLevels)
		{
			BuildDependencyLevels();
		}
		else
		{
			Uint64 max_level = passes.size();
			dependency_levels.reserve(max_level);
			for (Uint32 i = 0; i < max_level; ++i)
			{
				dependency_levels.emplace_back(*this, i);
				dependency_levels[i].AddPass(passes[i]);
			}
		}
		CullPasses();
		ResolveAsync();
		ResolveEvents();
		CalculateResourcesLifetime();
		for (DependencyLevel& dependency_level : dependency_levels)
		{
			dependency_level.Setup();
		}

		if (g_DumpRenderGraph)
		{
			Dump("rendergraph.gv");
		}
	}

	void RenderGraph::Execute()
	{
		ZoneScopedN("RenderGraph::Execute");
#if RG_MULTITHREADED
		Execute_Multithreaded();
#else
		Execute_Singlethreaded();
#endif
	}

	void RenderGraph::Execute_Singlethreaded()
	{
		pool.Tick();

		RenderGraphExecutionContext exec_ctx{};
		exec_ctx.gfx = gfx;
		exec_ctx.graphics_cmd_list = gfx->GetGraphicsCommandList();
		exec_ctx.compute_cmd_list = gfx->GetComputeCommandList();
		exec_ctx.graphics_fence = &gfx->GetGraphicsFence();
		exec_ctx.compute_fence = &gfx->GetComputeFence();
		exec_ctx.graphics_fence_value = gfx->GetGraphicsFenceValue();
		exec_ctx.compute_fence_value = gfx->GetComputeFenceValue();

		for (Uint64 i = 0; i < dependency_levels.size(); ++i)
		{
			DependencyLevel& dependency_level = dependency_levels[i];
			dependency_level.Execute(exec_ctx);
		}
	}

	void RenderGraph::Execute_Multithreaded()
	{
		ADRIA_ASSERT_MSG(false, "Not yet implemented!");
	}

	void RenderGraph::AddExportBufferCopyPass(RGResourceName export_buffer, GfxBuffer* buffer)
	{
#if RG_DEBUG
		std::string const buffer_copy_pass_name = "Export Buffer Copy Pass " + std::string(export_buffer.name);
#else 
		std::string const buffer_copy_pass_name = "Export Texture Copy Pass " + std::to_string(export_buffer.hashed_name);
#endif

		struct ExportBufferCopyPassData
		{
			RGBufferCopySrcId src;
		};

		AddPass<ExportBufferCopyPassData>(buffer_copy_pass_name.c_str(),
			[=](ExportBufferCopyPassData& data, RenderGraphBuilder& builder)
			{
				ADRIA_ASSERT(IsBufferDeclared(export_buffer));
				data.src = builder.ReadCopySrcBuffer(export_buffer);
			},
			[=](ExportBufferCopyPassData const& data, RenderGraphContext& context)
			{
				GfxCommandList* cmd_list = context.GetCommandList();
				GfxBuffer const& src_buffer = context.GetCopySrcBuffer(data.src);
				cmd_list->CopyBuffer(*buffer, src_buffer);
			}, RGPassType::Copy, RGPassFlags::ForceNoCull);
	}

	void RenderGraph::AddExportTextureCopyPass(RGResourceName export_texture, GfxTexture* texture)
	{
#if RG_DEBUG
		std::string const texture_copy_pass_name = "Export Texture Copy Pass " + std::string(export_texture.name);
#else 
		std::string const texture_copy_pass_name = "Export Texture Copy Pass " + std::to_string(export_texture.hashed_name);
#endif

		struct ExportTextureCopyPassData
		{
			RGTextureCopySrcId src;
		};

		AddPass<ExportTextureCopyPassData>(texture_copy_pass_name.c_str(),
			[=](ExportTextureCopyPassData& data, RenderGraphBuilder& builder)
			{
				ADRIA_ASSERT(IsTextureDeclared(export_texture));
				data.src = builder.ReadCopySrcTexture(export_texture);
			},
			[=](ExportTextureCopyPassData const& data, RenderGraphContext& context)
			{
				GfxCommandList* cmd_list = context.GetCommandList();
				GfxTexture const& src_texture = context.GetCopySrcTexture(data.src);
				cmd_list->CopyTexture(*texture, src_texture);
			}, RGPassType::Copy, RGPassFlags::ForceNoCull);
	}

	void RenderGraph::BuildAdjacencyLists()
	{
		adjacency_lists.resize(passes.size());
		for (Uint64 i = 0; i < passes.size(); ++i)
		{
			RGPassBase* pass = passes[i];
			std::vector<Uint64>& pass_adjacency_list = adjacency_lists[i];
			for (Uint64 j = i + 1; j < passes.size(); ++j)
			{
				RGPassBase* other_pass = passes[j];
				Bool depends = false;
				for (RGTextureId other_node_read : other_pass->texture_reads)
				{
					if (pass->texture_writes.find(other_node_read) != pass->texture_writes.end())
					{
						pass_adjacency_list.push_back(j);
						depends = true;
						break;
					}
				}

				if (depends)
				{
					continue;
				}

				for (RGBufferId other_node_read : other_pass->buffer_reads)
				{
					if (pass->buffer_writes.find(other_node_read) != pass->buffer_writes.end())
					{
						pass_adjacency_list.push_back(j);
						break;
					}
				}
			}
		}
	}

	void RenderGraph::TopologicalSort()
	{
		std::vector<Uint64> sort{};
		std::vector<Bool>  visited(passes.size(), false);
		for (Uint64 i = 0; i < passes.size(); i++)
		{
			if (visited[i] == false)
			{
				DepthFirstSearch(i, visited, topologically_sorted_passes);
			}
		}
		std::reverse(topologically_sorted_passes.begin(), topologically_sorted_passes.end());
	}

	void RenderGraph::BuildDependencyLevels()
	{
		std::vector<Uint64> distances(topologically_sorted_passes.size(), 0);
		for (Uint64 u = 0; u < topologically_sorted_passes.size(); ++u)
		{
			Uint64 i = topologically_sorted_passes[u];
			for (Uint64 v : adjacency_lists[i])
			{
				if (distances[v] < distances[i] + 1)
				{
					distances[v] = distances[i] + 1;
				}
			}
		}

		Uint64 max_level = *std::max_element(std::begin(distances), std::end(distances)) + 1;
		dependency_levels.reserve(max_level);
		for (Uint32 i = 0; i < max_level; ++i)
		{
			dependency_levels.emplace_back(*this, i); 
		}
		for (Uint32 i = 0; i < passes.size(); ++i)
		{
			Uint64 level = distances[i];
			dependency_levels[level].AddPass(passes[i]);
		}
	}

	void RenderGraph::CullPasses()
	{
		for (RGPassBase* pass : passes)
		{
			pass->ref_count = pass->texture_writes.size() + pass->buffer_writes.size();
			for (RGTextureId id : pass->texture_reads)
			{
				RGTexture* consumed = GetRGTexture(id);
				++consumed->ref_count;
			}
			for (RGBufferId id : pass->buffer_reads)
			{
				RGBuffer* consumed = GetRGBuffer(id);
				++consumed->ref_count;
			}

			for (RGTextureId id : pass->texture_writes)
			{
				RGTexture* written = GetRGTexture(id);
				written->writer = pass;
			}
			for (RGBufferId id : pass->buffer_writes)
			{
				RGBuffer* written = GetRGBuffer(id);
				written->writer = pass;
			}
		}

		if (!RGCullPasses.Get())
		{
			return;
		}

		std::stack<RenderGraphResource*> zero_ref_resources;
		for (auto& texture : textures)
		{
			if (texture->ref_count == 0)
			{
				zero_ref_resources.push(texture.get());
			}
		}
		for (auto& buffer : buffers)
		{
			if (buffer->ref_count == 0)
			{
				zero_ref_resources.push(buffer.get());
			}
		}

		while (!zero_ref_resources.empty())
		{
			RenderGraphResource* unreferenced_resource = zero_ref_resources.top();
			zero_ref_resources.pop();

			RGPassBase* writer = unreferenced_resource->writer;
			if (writer == nullptr || !writer->CanBeCulled())
			{
				continue;
			}

			if (--writer->ref_count == 0)
			{
				for (RGTextureId id : writer->texture_reads)
				{
					RGTexture* texture = GetRGTexture(id);
					if (--texture->ref_count == 0)
					{
						zero_ref_resources.push(texture);
					}
				}

				for (RGBufferId id : writer->buffer_reads)
				{
					RGBuffer* buffer = GetRGBuffer(id);
					if (--buffer->ref_count == 0)
					{
						zero_ref_resources.push(buffer);
					}
				}
			}
		}
	}

	void RenderGraph::CalculateResourcesLifetime()
	{
		for (auto& dependency_level : dependency_levels)
		{
			for (RGPassBase* pass : dependency_level.passes)
			{
				if (pass->IsCulled())
				{
					continue;
				}

				for (RGTextureId id : pass->texture_writes)
				{
					if (!pass->texture_state_map.contains(id))
					{
						continue;
					}

					RGTexture* rg_texture = GetRGTexture(id);
					rg_texture->last_used_by = pass;
				}

				for (RGBufferId id : pass->buffer_writes)
				{
					if (!pass->buffer_state_map.contains(id))
					{
						continue;
					}

					RGBuffer* rg_buffer = GetRGBuffer(id);
					rg_buffer->last_used_by = pass;
				}

				for (RGTextureId id : pass->texture_reads)
				{
					if (!pass->texture_state_map.contains(id))
					{
						continue;
					}

					RGTexture* rg_texture = GetRGTexture(id);
					rg_texture->last_used_by = pass;
				}

				for (RGBufferId id : pass->buffer_reads)
				{
					if (!pass->buffer_state_map.contains(id))
					{
						continue;
					}

					RGBuffer* rg_buffer = GetRGBuffer(id);
					rg_buffer->last_used_by = pass;
				}
			}
		}

		for (Uint64 i = 0; i < textures.size(); ++i)
		{
			if (textures[i]->last_used_by != nullptr)
			{
				textures[i]->last_used_by->texture_destroys.insert(RGTextureId(i));
			}

			if (textures[i]->imported)
			{
				CreateTextureViews(RGTextureId(i));
			}
		}
		for (Uint64 i = 0; i < buffers.size(); ++i)
		{
			if (buffers[i]->last_used_by != nullptr)
			{
				buffers[i]->last_used_by->buffer_destroys.insert(RGBufferId(i));
			}

			if (buffers[i]->imported)
			{
				CreateBufferViews(RGBufferId(i));
			}
		}
	}

	void RenderGraph::DepthFirstSearch(Uint64 i, std::vector<Bool>& visited, std::vector<Uint64>& topologically_sorted_passes)
	{
		visited[i] = true;
		for (auto j : adjacency_lists[i])
		{
			if (!visited[j])
			{
				DepthFirstSearch(j, visited, topologically_sorted_passes);
			}
		}
		topologically_sorted_passes.push_back(i);
	}

	void RenderGraph::ResolveAsync()
	{
#if GFX_ASYNC_COMPUTE
		if (!RGAsyncCompute.Get())
		{
			return;
		}

		std::vector<RGPassBase*> compute_queue_passes;
		std::unordered_set<RGPassBase*> pre_graphics_queue_passes;
		std::unordered_set<RGPassBase*> post_graphics_queue_passes;
		Uint64 compute_fence = 0;
		Uint64 graphics_fence = 0;

		for (Uint64 pass_index : topologically_sorted_passes)
		{
			RGPassBase* pass = passes[pass_index];
			if (pass->IsCulled())
			{
				continue;
			}

			if (pass->type == RGPassType::AsyncCompute)
			{
				for (RGTextureId read_texture : pass->texture_reads)
				{
					for (Int64 i = (Int64)pass_index - 1; i >= 0; --i)
					{
						RGPassBase* pre_pass = passes[i];
						if (pre_pass->IsCulled() || pre_pass->type == RGPassType::AsyncCompute)
						{
							continue;
						}

						if (pre_pass->texture_writes.find(read_texture) != pre_pass->texture_writes.end())
						{
							pre_graphics_queue_passes.insert(pre_pass);
							break;
						}
					}
				}
				for (RGBufferId read_buffer : pass->buffer_reads)
				{
					for (Int64 i = (Int64)pass_index - 1; i >= 0; --i)
					{
						RGPassBase* pre_pass = passes[i];
						if (pre_pass->IsCulled() || pre_pass->type == RGPassType::AsyncCompute)
						{
							continue;
						}

						if (pre_pass->buffer_writes.find(read_buffer) != pre_pass->buffer_writes.end())
						{
							pre_graphics_queue_passes.insert(pre_pass);
							break;
						}
					}
				}
				for (RGTextureId write_texture : pass->texture_writes)
				{
					for (Uint64 i = pass_index + 1; i < passes.size(); ++i)
					{
						RGPassBase* post_pass = passes[i];
						if (post_pass->IsCulled() || post_pass->type == RGPassType::AsyncCompute)
						{
							continue;
						}

						if (post_pass->texture_reads.find(write_texture) != post_pass->texture_reads.end())
						{
							post_graphics_queue_passes.insert(post_pass);
							break;
						}
					}
				}
				for (RGBufferId write_buffer : pass->buffer_writes)
				{
					for (Uint64 i = pass_index + 1; i < passes.size(); ++i)
					{
						RGPassBase* post_pass = passes[i];
						if (post_pass->IsCulled() || post_pass->type == RGPassType::AsyncCompute)
						{
							continue;
						}

						if (post_pass->buffer_reads.find(write_buffer) != post_pass->buffer_reads.end())
						{
							pre_graphics_queue_passes.insert(post_pass);
							break;
						}
					}
				}
				compute_queue_passes.push_back(pass);
			}
			else if (!compute_queue_passes.empty())
			{
				if (!pre_graphics_queue_passes.empty())
				{
					RGPassBase* last_pre_pass = *std::max_element(
						pre_graphics_queue_passes.begin(),
						pre_graphics_queue_passes.end(),
						[](RGPassBase* a, RGPassBase* b) { return a->id < b->id; }
					);

					if (last_pre_pass->signal_value == Uint64(-1))
					{
						last_pre_pass->signal_value = ++graphics_fence;
					}
					RGPassBase* first_compute_pass = compute_queue_passes.front();
					first_compute_pass->wait_value = last_pre_pass->signal_value;
					first_compute_pass->wait_graphics_pass_id = last_pre_pass->id;
				}

				if (!post_graphics_queue_passes.empty())
				{
					RGPassBase* first_post_pass = *std::min_element(
						post_graphics_queue_passes.begin(),
						post_graphics_queue_passes.end(),
						[](RGPassBase* a, RGPassBase* b) { return a->id < b->id; }
					);

					RGPassBase* last_compute_pass = compute_queue_passes.back();
					if (last_compute_pass->signal_value == Uint64(-1))
					{
						last_compute_pass->signal_value = ++compute_fence;
					}

					first_post_pass->wait_value = last_compute_pass->signal_value;
					last_compute_pass->signal_graphics_pass_id = first_post_pass->id;
				}
				compute_queue_passes.clear();
				pre_graphics_queue_passes.clear();
				post_graphics_queue_passes.clear();
			}
		}
#endif
	}

	void RenderGraph::ResolveEvents()
	{
		std::vector<Uint32> events_to_start;
		Uint32 events_to_add = 0;
		RGPassBase* last_active_pass = nullptr;
		for (RGPassBase* const pass : passes)
		{
			if (pass->IsCulled())
			{
				while (pass->num_events_to_end > 0 && pass->events_to_start.size() > 0)
				{
					pass->num_events_to_end--;
					pass->events_to_start.pop_back();
				}
				for (Uint32 event_idx : pass->events_to_start)
				{
					events_to_start.push_back(event_idx);
				}
				events_to_add += pass->num_events_to_end;
			}
			else
			{
				for (Uint32 eventIndex : events_to_start)
				{
					pass->events_to_start.push_back(eventIndex);
				}
				pass->num_events_to_end += events_to_add;
				events_to_start.clear();
				events_to_add = 0;
				last_active_pass = pass;
			}
		}
		if (last_active_pass)
		{
			last_active_pass->num_events_to_end += events_to_add;
		}
		ADRIA_ASSERT(events_to_start.empty());
	}

	RGTexture* RenderGraph::GetRGTexture(RGTextureId handle) const
	{
		return textures[handle.id].get();
	}

	RGBuffer* RenderGraph::GetRGBuffer(RGBufferId handle) const
	{
		return buffers[handle.id].get();
	}

	GfxTexture* RenderGraph::GetTexture(RGTextureId res_id) const
	{
		return GetRGTexture(res_id)->resource;
	}

	GfxBuffer* RenderGraph::GetBuffer(RGBufferId res_id) const
	{
		return GetRGBuffer(res_id)->resource;
	}

	void RenderGraph::CreateTextureViews(RGTextureId res_id)
	{
		auto const& view_descs = texture_view_desc_map[res_id];
		for (auto const& [view_desc, type] : view_descs)
		{
			GfxTexture* texture = GetTexture(res_id);
			GfxDescriptor view;
			switch (type)
			{
			case RGDescriptorType::RenderTarget:
				view = gfx->CreateTextureRTV(texture, &view_desc);
				break;
			case RGDescriptorType::DepthStencil:
				view = gfx->CreateTextureDSV(texture, &view_desc);
				break;
			case RGDescriptorType::ReadOnly:
				view = gfx->CreateTextureSRV(texture, &view_desc);
				break;
			case RGDescriptorType::ReadWrite:
				view = gfx->CreateTextureUAV(texture, &view_desc);
				break;
			default:
				ADRIA_ASSERT_MSG(false, "invalid resource view type for texture");
			}
			texture_view_map[res_id].push_back(view);
		}
	}

	void RenderGraph::CreateBufferViews(RGBufferId res_id)
	{
		auto const& view_descs = buffer_view_desc_map[res_id];
		for (Uint64 i = 0; i < view_descs.size(); ++i)
		{
			auto const& [view_desc, type] = view_descs[i];
			GfxBuffer* buffer = GetBuffer(res_id);
			GfxDescriptor view;
			switch (type)
			{
			case RGDescriptorType::ReadOnly:
			{
				view = gfx->CreateBufferSRV(buffer, &view_desc);
				break;
			}
			case RGDescriptorType::ReadWrite:
			{
				RGBufferReadWriteId rw_id(i, res_id);
				if (buffer_uav_counter_map.contains(rw_id))
				{
					GfxBuffer* counter_buffer = GetBuffer(buffer_uav_counter_map[rw_id]);
					view = gfx->CreateBufferUAV(buffer, counter_buffer, &view_desc);
				}
				else
				{
					view = gfx->CreateBufferUAV(buffer, &view_desc);
				}
				break;
			}
			case RGDescriptorType::RenderTarget:
			case RGDescriptorType::DepthStencil:
			default:
				ADRIA_ASSERT_MSG(false, "invalid resource view type for buffer");
			}
			buffer_view_map[res_id].emplace_back(view, type);
		}
	}

	RGTextureId RenderGraph::GetTextureId(RGResourceName name)
	{
		ADRIA_ASSERT(IsTextureDeclared(name));
		return texture_name_id_map[name];
	}

	RGBufferId RenderGraph::GetBufferId(RGResourceName name)
	{
		ADRIA_ASSERT(IsBufferDeclared(name));
		return buffer_name_id_map[name];
	}

	RGTextureDesc RenderGraph::GetTextureDesc(RGResourceName name)
	{
		ADRIA_ASSERT(IsTextureDeclared(name));
		RGTextureId tex_id = texture_name_id_map[name];
		RGTextureDesc desc{};
		ExtractRGTextureDesc(GetRGTexture(tex_id)->desc, desc);
		return desc;
	}

	RGBufferDesc RenderGraph::GetBufferDesc(RGResourceName name)
	{
		ADRIA_ASSERT(IsBufferDeclared(name));
		RGBufferId buf_id = buffer_name_id_map[name];
		RGBufferDesc desc{};
		ExtractRGBufferDesc(GetRGBuffer(buf_id)->desc, desc);
		return desc;
	}

	void RenderGraph::AddBufferBindFlags(RGResourceName name, GfxBindFlag flags)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		RGBuffer* rg_buffer = GetRGBuffer(handle);
		rg_buffer->desc.bind_flags |= flags;
	}

	void RenderGraph::AddTextureBindFlags(RGResourceName name, GfxBindFlag flags)
	{
		RGTextureId handle = texture_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidTextureHandle(handle), "Resource has not been declared!");
		RGTexture* rg_texture = GetRGTexture(handle);
		rg_texture->desc.bind_flags |= flags;
	}

	RGTextureCopySrcId RenderGraph::ReadCopySrcTexture(RGResourceName name)
	{
		RGTextureId handle = texture_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidTextureHandle(handle), "Resource has not been declared!");
		RGTexture* rg_texture = GetRGTexture(handle);
		if (rg_texture->desc.initial_state == GfxResourceState::Common)
		{
			rg_texture->desc.initial_state = GfxResourceState::CopyDst;
		}
		return RGTextureCopySrcId(handle);
	}

	RGTextureCopyDstId RenderGraph::WriteCopyDstTexture(RGResourceName name)
	{
		RGTextureId handle = texture_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidTextureHandle(handle), "Resource has not been declared!");
		RGTexture* rg_texture = GetRGTexture(handle);
		if (rg_texture->desc.initial_state == GfxResourceState::Common)
		{
			rg_texture->desc.initial_state = GfxResourceState::CopyDst;
		}
		return RGTextureCopyDstId(handle);
	}

	RGBufferCopySrcId RenderGraph::ReadCopySrcBuffer(RGResourceName name)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		return RGBufferCopySrcId(handle);
	}

	RGBufferCopyDstId RenderGraph::WriteCopyDstBuffer(RGResourceName name)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		return RGBufferCopyDstId(handle);
	}

	RGBufferIndirectArgsId RenderGraph::ReadIndirectArgsBuffer(RGResourceName name)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		return RGBufferIndirectArgsId(handle);
	}

	RGBufferVertexId RenderGraph::ReadVertexBuffer(RGResourceName name)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		return RGBufferVertexId(handle);
	}

	RGBufferIndexId RenderGraph::ReadIndexBuffer(RGResourceName name)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		return RGBufferIndexId(handle);
	}

	RGBufferConstantId RenderGraph::ReadConstantBuffer(RGResourceName name)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		return RGBufferConstantId(handle);
	}

	RGRenderTargetId RenderGraph::RenderTarget(RGResourceName name, GfxTextureDescriptorDesc const& desc)
	{
		RGTextureId handle = texture_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidTextureHandle(handle), "Resource has not been declared!");
		RGTexture* rg_texture = GetRGTexture(handle);
		rg_texture->desc.bind_flags |= GfxBindFlag::RenderTarget;
		if (rg_texture->desc.initial_state == GfxResourceState::Common)
		{
			rg_texture->desc.initial_state = GfxResourceState::RTV;
		}
		std::vector<std::pair<GfxTextureDescriptorDesc, RGDescriptorType>>& view_descs = texture_view_desc_map[handle];
		for (Uint64 i = 0; i < view_descs.size(); ++i)
		{
			auto const& [_desc, _type] = view_descs[i];
			if (desc == _desc && _type == RGDescriptorType::RenderTarget)
			{
				return RGRenderTargetId(i, handle);
			}
		}
		Uint64 view_id = view_descs.size();
		view_descs.emplace_back(desc, RGDescriptorType::RenderTarget);
		return RGRenderTargetId(view_id, handle);
	}

	RGDepthStencilId RenderGraph::DepthStencil(RGResourceName name, GfxTextureDescriptorDesc const& desc)
	{
		RGTextureId handle = texture_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidTextureHandle(handle), "Resource has not been declared!");
		RGTexture* rg_texture = GetRGTexture(handle);
		rg_texture->desc.bind_flags |= GfxBindFlag::DepthStencil;
		if (rg_texture->desc.initial_state == GfxResourceState::Common)
		{
			rg_texture->desc.initial_state = GfxResourceState::DSV;
		}
		std::vector<std::pair<GfxTextureDescriptorDesc, RGDescriptorType>>& view_descs = texture_view_desc_map[handle];
		for (Uint64 i = 0; i < view_descs.size(); ++i)
		{
			auto const& [_desc, _type] = view_descs[i];
			if (desc == _desc && _type == RGDescriptorType::DepthStencil)
			{
				return RGDepthStencilId(i, handle);
			}
		}
		Uint64 view_id = view_descs.size();
		view_descs.emplace_back(desc, RGDescriptorType::DepthStencil);
		return RGDepthStencilId(view_id, handle);
	}

	RGTextureReadOnlyId RenderGraph::ReadTexture(RGResourceName name, GfxTextureDescriptorDesc const& desc)
	{
		RGTextureId handle = texture_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidTextureHandle(handle), "Resource has not been declared!");
		RGTexture* rg_texture = GetRGTexture(handle);
		rg_texture->desc.bind_flags |= GfxBindFlag::ShaderResource;
		if (rg_texture->desc.initial_state == GfxResourceState::Common)
		{
			rg_texture->desc.initial_state = GfxResourceState::PixelSRV | GfxResourceState::ComputeSRV;
		}
		std::vector<std::pair<GfxTextureDescriptorDesc, RGDescriptorType>>& view_descs = texture_view_desc_map[handle];
		for (Uint64 i = 0; i < view_descs.size(); ++i)
		{
			auto const& [_desc, _type] = view_descs[i];
			if (desc == _desc && _type == RGDescriptorType::ReadOnly)
			{
				return RGTextureReadOnlyId(i, handle);
			}
		}
		Uint64 view_id = view_descs.size();
		view_descs.emplace_back(desc, RGDescriptorType::ReadOnly);
		return RGTextureReadOnlyId(view_id, handle);
	}

	RGTextureReadWriteId RenderGraph::WriteTexture(RGResourceName name, GfxTextureDescriptorDesc const& desc)
	{
		RGTextureId handle = texture_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidTextureHandle(handle), "Resource has not been declared!");
		RGTexture* rg_texture = GetRGTexture(handle);
		rg_texture->desc.bind_flags |= GfxBindFlag::UnorderedAccess;
		if (rg_texture->desc.initial_state == GfxResourceState::Common)
		{
			rg_texture->desc.initial_state = GfxResourceState::AllUAV;
		}
		std::vector<std::pair<GfxTextureDescriptorDesc, RGDescriptorType>>& view_descs = texture_view_desc_map[handle];
		for (Uint64 i = 0; i < view_descs.size(); ++i)
		{
			auto const& [_desc, _type] = view_descs[i];
			if (desc == _desc && _type == RGDescriptorType::ReadWrite)
			{
				return RGTextureReadWriteId(i, handle);
			}
		}
		Uint64 view_id = view_descs.size();
		view_descs.emplace_back(desc, RGDescriptorType::ReadWrite);
		return RGTextureReadWriteId(view_id, handle);
	}

	RGBufferReadOnlyId RenderGraph::ReadBuffer(RGResourceName name, GfxBufferDescriptorDesc const& desc)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		RGBuffer* rg_buffer = GetRGBuffer(handle);
		rg_buffer->desc.bind_flags |= GfxBindFlag::ShaderResource;
		std::vector<std::pair<GfxBufferDescriptorDesc, RGDescriptorType>>& view_descs = buffer_view_desc_map[handle];
		for (Uint64 i = 0; i < view_descs.size(); ++i)
		{
			auto const& [_desc, _type] = view_descs[i];
			if (desc == _desc && _type == RGDescriptorType::ReadOnly)
			{
				return RGBufferReadOnlyId(i, handle);
			}
		}
		Uint64 view_id = view_descs.size();
		view_descs.emplace_back(desc, RGDescriptorType::ReadOnly);
		return RGBufferReadOnlyId(view_id, handle);
	}

	RGBufferReadWriteId RenderGraph::WriteBuffer(RGResourceName name, GfxBufferDescriptorDesc const& desc)
	{
		RGBufferId handle = buffer_name_id_map[name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		RGBuffer* rg_buffer = GetRGBuffer(handle);
		rg_buffer->desc.bind_flags |= GfxBindFlag::UnorderedAccess;
		std::vector<std::pair<GfxBufferDescriptorDesc, RGDescriptorType>>& view_descs = buffer_view_desc_map[handle];
		for (Uint64 i = 0; i < view_descs.size(); ++i)
		{
			auto const& [_desc, _type] = view_descs[i];
			if (desc == _desc && _type == RGDescriptorType::ReadWrite)
			{
				return RGBufferReadWriteId(i, handle);
			}
		}
		Uint64 view_id = view_descs.size();
		view_descs.emplace_back(desc, RGDescriptorType::ReadWrite);
		return RGBufferReadWriteId(view_id, handle);
	}

	RGBufferReadWriteId RenderGraph::WriteBuffer(RGResourceName name, RGResourceName counter_name, GfxBufferDescriptorDesc const& desc)
	{
		RGBufferId handle = buffer_name_id_map[name];
		RGBufferId counter_handle = buffer_name_id_map[counter_name];
		ADRIA_ASSERT_MSG(IsValidBufferHandle(handle), "Resource has not been declared!");
		ADRIA_ASSERT_MSG(IsValidBufferHandle(counter_handle), "Resource has not been declared!");

		RGBuffer* rg_buffer = GetRGBuffer(handle);
		RGBuffer* rg_counter_buffer = GetRGBuffer(counter_handle);

		rg_buffer->desc.bind_flags |= GfxBindFlag::UnorderedAccess;
		rg_counter_buffer->desc.bind_flags |= GfxBindFlag::UnorderedAccess;

		std::vector<std::pair<GfxBufferDescriptorDesc, RGDescriptorType>>& view_descs = buffer_view_desc_map[handle];
		for (Uint64 i = 0; i < view_descs.size(); ++i)
		{
			auto const& [_desc, _type] = view_descs[i];
			if (desc == _desc && _type == RGDescriptorType::ReadWrite)
			{
				RGBufferReadWriteId rw_id(i, handle);
				if (auto it = buffer_uav_counter_map.find(rw_id); it != buffer_uav_counter_map.end())
				{
					if (it->second == counter_handle)
					{
						return rw_id;
					}
				}
			}
		}
		Uint64 view_id = view_descs.size();
		view_descs.emplace_back(desc, RGDescriptorType::ReadWrite);
		RGBufferReadWriteId rw_id = RGBufferReadWriteId(view_id, handle);
		buffer_uav_counter_map.insert(std::make_pair(rw_id, counter_handle));
		return rw_id;
	}

	GfxTexture const& RenderGraph::GetCopySrcTexture(RGTextureCopySrcId res_id) const
	{
		return *GetTexture(RGTextureId(res_id));
	}

	GfxTexture& RenderGraph::GetCopyDstTexture(RGTextureCopyDstId res_id) const
	{
		return *GetTexture(RGTextureId(res_id));
	}

	GfxBuffer const& RenderGraph::GetCopySrcBuffer(RGBufferCopySrcId res_id) const
	{
		return *GetBuffer(RGBufferId(res_id));
	}

	GfxBuffer& RenderGraph::GetCopyDstBuffer(RGBufferCopyDstId res_id) const
	{
		return *GetBuffer(RGBufferId(res_id));
	}

	GfxBuffer const& RenderGraph::GetIndirectArgsBuffer(RGBufferIndirectArgsId res_id) const
	{
		return *GetBuffer(RGBufferId(res_id));
	}

	GfxBuffer const& RenderGraph::GetVertexBuffer(RGBufferVertexId res_id) const
	{
		return *GetBuffer(RGBufferId(res_id));
	}

	GfxBuffer const& RenderGraph::GetIndexBuffer(RGBufferIndexId res_id) const
	{
		return *GetBuffer(RGBufferId(res_id));
	}

	GfxBuffer const& RenderGraph::GetConstantBuffer(RGBufferConstantId res_id) const
	{
		return *GetBuffer(RGBufferId(res_id));
	}

	GfxDescriptor RenderGraph::GetRenderTarget(RGRenderTargetId res_id) const
	{
		RGTextureId tex_id = res_id.GetResourceId();
		auto const& views = texture_view_map[tex_id];
		return views[res_id.GetViewId()];
	}

	GfxDescriptor RenderGraph::GetDepthStencil(RGDepthStencilId res_id) const
	{
		RGTextureId tex_id = res_id.GetResourceId();
		auto const& views = texture_view_map[tex_id];
		return views[res_id.GetViewId()];
	}

	GfxDescriptor RenderGraph::GetReadOnlyTexture(RGTextureReadOnlyId res_id) const
	{
		RGTextureId tex_id = res_id.GetResourceId();
		auto const& views = texture_view_map[tex_id];
		return views[res_id.GetViewId()];
	}

	GfxDescriptor RenderGraph::GetReadWriteTexture(RGTextureReadWriteId res_id) const
	{
		RGTextureId tex_id = res_id.GetResourceId();
		auto const& views = texture_view_map[tex_id];
		return views[res_id.GetViewId()];
	}

	GfxDescriptor RenderGraph::GetReadOnlyBuffer(RGBufferReadOnlyId res_id) const
	{
		RGBufferId buf_id = res_id.GetResourceId();
		auto const& views = buffer_view_map[buf_id];
		return views[res_id.GetViewId()];
	}

	GfxDescriptor RenderGraph::GetReadWriteBuffer(RGBufferReadWriteId res_id) const
	{
		RGBufferId buf_id = res_id.GetResourceId();
		auto const& views = buffer_view_map[buf_id];
		return views[res_id.GetViewId()];
	}

	void RenderGraph::DependencyLevel::AddPass(RenderGraphPassBase* pass)
	{
		passes.push_back(pass);
		texture_reads.insert(pass->texture_reads.begin(), pass->texture_reads.end());
		texture_writes.insert(pass->texture_writes.begin(), pass->texture_writes.end());
		buffer_reads.insert(pass->buffer_reads.begin(), pass->buffer_reads.end());
		buffer_writes.insert(pass->buffer_writes.begin(), pass->buffer_writes.end());
	}

	void RenderGraph::DependencyLevel::Setup()
	{
		for (RGPassBase* pass : passes)
		{
			if (pass->IsCulled())
			{
				continue;
			}

			texture_creates.insert(pass->texture_creates.begin(), pass->texture_creates.end());
			texture_destroys.insert(pass->texture_destroys.begin(), pass->texture_destroys.end());
			for (auto [resource, state] : pass->texture_state_map)
			{
				texture_state_map[resource] |= state;
			}

			buffer_creates.insert(pass->buffer_creates.begin(), pass->buffer_creates.end());
			buffer_destroys.insert(pass->buffer_destroys.begin(), pass->buffer_destroys.end());
			for (auto [resource, state] : pass->buffer_state_map)
			{
				buffer_state_map[resource] |= state;
			}
		}
	}

	void RenderGraph::DependencyLevel::Execute(RenderGraphExecutionContext const& exec_ctx)
	{
		PreExecute(exec_ctx.graphics_cmd_list);
		for (auto& pass : passes)
		{
			if (pass->IsCulled())
			{
				continue;
			}

#if GFX_ASYNC_COMPUTE
			GfxCommandList* cmd_list = pass->type == RGPassType::AsyncCompute && RGAsyncCompute.Get() ? exec_ctx.compute_cmd_list : exec_ctx.graphics_cmd_list;
#else
			GfxCommandList* cmd_list = exec_ctx.graphics_cmd_list;
#endif
			if (pass->wait_value != UINT64_MAX)
			{
				cmd_list->End();
				cmd_list->Submit();
				cmd_list->Begin();
				if (pass->type == RGPassType::AsyncCompute)
				{
					cmd_list->Wait(*exec_ctx.graphics_fence, exec_ctx.graphics_fence_value + pass->wait_value);
				}
				else
				{
					cmd_list->Wait(*exec_ctx.compute_fence, exec_ctx.compute_fence_value + pass->wait_value);
				}
			}

			for (Uint32 event_idx : pass->events_to_start)
			{
				cmd_list->BeginEvent(rg.events[event_idx].name, GfxEventColor(0x5E, 0xC4, 0xFF));
			}

			RenderGraphContext render_graph_ctx(rg, *pass, cmd_list);
			if (pass->type == RGPassType::Graphics)
			{
				GfxRenderPassDesc render_pass_desc{};
				render_pass_desc.flags = GfxRenderPassFlagBit_None;
				render_pass_desc.rtv_attachments.reserve(pass->render_targets_info.size());
				for (auto const& render_target_info : pass->render_targets_info)
				{
					GfxColorAttachmentDesc rtv_desc{};

					RGLoadAccessOp load_access = RGLoadAccessOp::NoAccess;
					RGStoreAccessOp store_access = RGStoreAccessOp::NoAccess;
					SplitAccessOp(render_target_info.render_target_access, load_access, store_access);

					switch (load_access)
					{
					case RGLoadAccessOp::Clear:
						rtv_desc.beginning_access = GfxLoadAccessOp::Clear;
						break;
					case RGLoadAccessOp::Discard:
						rtv_desc.beginning_access = GfxLoadAccessOp::Discard;
						break;
					case RGLoadAccessOp::Preserve:
						rtv_desc.beginning_access = GfxLoadAccessOp::Preserve;
						break;
					case RGLoadAccessOp::NoAccess:
						rtv_desc.beginning_access = GfxLoadAccessOp::NoAccess;
						break;
					default:
						ADRIA_ASSERT_MSG(false, "Invalid Load Access!");
					}

					switch (store_access)
					{
					case RGStoreAccessOp::Resolve:
						rtv_desc.ending_access = GfxStoreAccessOp::Resolve;
						break;
					case RGStoreAccessOp::Discard:
						rtv_desc.ending_access = GfxStoreAccessOp::Discard;
						break;
					case RGStoreAccessOp::Preserve:
						rtv_desc.ending_access = GfxStoreAccessOp::Preserve;
						break;
					case RGStoreAccessOp::NoAccess:
						rtv_desc.ending_access = GfxStoreAccessOp::NoAccess;
						break;
					default:
						ADRIA_ASSERT_MSG(false, "Invalid Store Access!");
					}

					RGTextureId rt_texture = render_target_info.render_target_handle.GetResourceId();
					GfxTexture* texture = rg.GetTexture(rt_texture);

					GfxTextureDesc const& desc = texture->GetDesc();
					GfxClearValue const& clear_value = desc.clear_value;
					if (clear_value.active_member != GfxClearValue::GfxActiveMember::None)
					{
						ADRIA_ASSERT_MSG(clear_value.active_member == GfxClearValue::GfxActiveMember::Color, "Invalid Clear Value for Render Target");
						rtv_desc.clear_value = desc.clear_value;
						rtv_desc.clear_value.format = desc.format;
					}
					else if(rtv_desc.beginning_access == GfxLoadAccessOp::Clear)
					{
						rtv_desc.clear_value.format = desc.format;
						rtv_desc.clear_value = GfxClearValue(0.0f, 0.0f, 0.0f, 0.0f);
					}

					rtv_desc.cpu_handle = rg.GetRenderTarget(render_target_info.render_target_handle);
					render_pass_desc.rtv_attachments.push_back(rtv_desc);
				}

				if (pass->depth_stencil.has_value())
				{
					auto const& depth_stencil_info = pass->depth_stencil.value();
					if (depth_stencil_info.depth_read_only)
					{
						render_pass_desc.flags |= GfxRenderPassFlagBit_ReadOnlyDepth;
					}
					
					GfxDepthAttachmentDesc dsv_desc{};
					RGLoadAccessOp load_access = RGLoadAccessOp::NoAccess;
					RGStoreAccessOp store_access = RGStoreAccessOp::NoAccess;
					SplitAccessOp(depth_stencil_info.depth_access, load_access, store_access);

					switch (load_access)
					{
					case RGLoadAccessOp::Clear:
						dsv_desc.depth_beginning_access = GfxLoadAccessOp::Clear;
						break;
					case RGLoadAccessOp::Discard:
						dsv_desc.depth_beginning_access = GfxLoadAccessOp::Discard;
						break;
					case RGLoadAccessOp::Preserve:
						dsv_desc.depth_beginning_access = GfxLoadAccessOp::Preserve;
						break;
					case RGLoadAccessOp::NoAccess:
						dsv_desc.depth_beginning_access = GfxLoadAccessOp::NoAccess;
						break;
					default:
						ADRIA_ASSERT_MSG(false, "Invalid Load Access!");
					}

					switch (store_access)
					{
					case RGStoreAccessOp::Resolve:
						dsv_desc.depth_ending_access = GfxStoreAccessOp::Resolve;
						break;
					case RGStoreAccessOp::Discard:
						dsv_desc.depth_ending_access = GfxStoreAccessOp::Discard;
						break;
					case RGStoreAccessOp::Preserve:
						dsv_desc.depth_ending_access = GfxStoreAccessOp::Preserve;
						break;
					case RGStoreAccessOp::NoAccess:
						dsv_desc.depth_ending_access = GfxStoreAccessOp::NoAccess;
						break;
					default:
						ADRIA_ASSERT_MSG(false, "Invalid Store Access!");
					}

					RGTextureId ds_texture = depth_stencil_info.depth_stencil_handle.GetResourceId();
					GfxTexture* texture = rg.GetTexture(ds_texture);

					GfxTextureDesc const& desc = texture->GetDesc();
					if (desc.clear_value.active_member != GfxClearValue::GfxActiveMember::None)
					{
						ADRIA_ASSERT_MSG(desc.clear_value.active_member == GfxClearValue::GfxActiveMember::DepthStencil, "Invalid Clear Value for Depth Stencil");
						dsv_desc.clear_value = desc.clear_value;
						dsv_desc.clear_value.format = desc.format;
					}
					else if (dsv_desc.depth_beginning_access == GfxLoadAccessOp::Clear)
					{
						dsv_desc.clear_value.format = desc.format;
						dsv_desc.clear_value = GfxClearValue(0.0f, 0);
					}

					dsv_desc.cpu_handle = rg.GetDepthStencil(depth_stencil_info.depth_stencil_handle);

					ADRIA_TODO("Add Stencil Support");
					render_pass_desc.dsv_attachment = dsv_desc;
				}
				ADRIA_ASSERT_MSG((pass->viewport_width != 0 && pass->viewport_height != 0), "Viewport Width/Height is 0! The call to builder.SetViewport is probably missing...");
				render_pass_desc.width = pass->viewport_width;
				render_pass_desc.height = pass->viewport_height;
				render_pass_desc.legacy = pass->UseLegacyRenderPasses();

				ZoneTransientN(__tracy, pass->name.c_str(), true);
				AdriaGfxScopedEvent(cmd_list, pass->name.c_str());
				cmd_list->SetContext(GfxCommandList::Context::Graphics);
				cmd_list->BeginRenderPass(render_pass_desc);
				pass->Execute(render_graph_ctx);
				cmd_list->EndRenderPass();
			}
			else
			{
				ZoneTransientN(__tracy, pass->name.c_str(), true);
				AdriaGfxScopedEvent(cmd_list, pass->name.c_str());
				cmd_list->SetContext(GfxCommandList::Context::Compute);
				pass->Execute(render_graph_ctx);
			}

			for (Uint32 i = 0; i < pass->num_events_to_end; ++i)
			{
				cmd_list->EndEvent();
			}

			if (pass->signal_value != UINT64_MAX)
			{
				cmd_list->End();
				if (pass->type == RGPassType::AsyncCompute)
				{
					cmd_list->Signal(*exec_ctx.compute_fence, exec_ctx.compute_fence_value + pass->signal_value);
					exec_ctx.gfx->SetComputeFenceValue(exec_ctx.compute_fence_value + pass->signal_value);
				}
				else
				{
					cmd_list->Signal(*exec_ctx.graphics_fence, exec_ctx.graphics_fence_value + pass->signal_value);
					exec_ctx.gfx->SetGraphicsFenceValue(exec_ctx.graphics_fence_value + pass->signal_value);
				}
				cmd_list->Submit();
				cmd_list->Begin();
			}
		} 
		PostExecute(exec_ctx.graphics_cmd_list);
	}

	void RenderGraph::DependencyLevel::PreExecute(GfxCommandList* cmd_list)
	{
		for (RGTextureId tex_id : texture_creates)
		{
			RGTexture* rg_texture = rg.GetRGTexture(tex_id);
			if (!rg_texture->imported)
			{
				rg_texture->resource = rg.pool.AllocateTexture(rg_texture->desc);
			}
			rg.CreateTextureViews(tex_id);
			rg_texture->SetName();
		}
		for (RGBufferId buf_id : buffer_creates)
		{
			RGBuffer* rg_buffer = rg.GetRGBuffer(buf_id);
			if (!rg_buffer->imported)
			{
				rg_buffer->resource = rg.pool.AllocateBuffer(rg_buffer->desc);
			}
			rg.CreateBufferViews(buf_id);
			rg_buffer->SetName();
		}

		for (auto const& [tex_id, state] : texture_state_map)
		{
			RGTexture* rg_texture = rg.GetRGTexture(tex_id);
			GfxTexture* texture = rg_texture->resource;
			if (texture_creates.contains(tex_id))
			{
				if (!HasFlag(texture->GetDesc().initial_state, state))
				{
					cmd_list->TextureBarrier(*texture, texture->GetDesc().initial_state, state);
				}
				continue;
			}
			Bool found = false;
			for (Int32 j = (Int32)level_index - 1; j >= 0; --j)
			{
				auto& prev_dependency_level = rg.dependency_levels[j];
				if (prev_dependency_level.texture_state_map.contains(tex_id))
				{
					GfxResourceState prev_state = prev_dependency_level.texture_state_map[tex_id];
					if (prev_state != state)
					{
						cmd_list->TextureBarrier(*texture, prev_state, state);
					}
					found = true;
					break;
				}
			}
			if (!found && rg_texture->imported)
			{
				GfxResourceState prev_state = rg_texture->desc.initial_state;
				if (prev_state != state)
				{
					cmd_list->TextureBarrier(*texture, prev_state, state);
				}
			}
		}
		for (auto const& [buf_id, state] : buffer_state_map)
		{
			RGBuffer* rg_buffer = rg.GetRGBuffer(buf_id);
			GfxBuffer* buffer = rg_buffer->resource;
			if (buffer_creates.contains(buf_id))
			{
				if (state != GfxResourceState::Common)
				{
					cmd_list->BufferBarrier(*buffer, GfxResourceState::Common, state);
				}
				continue;
			}
			Bool found = false;
			for (Int32 j = (Int32)level_index - 1; j >= 0; --j)
			{
				auto& prev_dependency_level = rg.dependency_levels[j];
				if (prev_dependency_level.buffer_state_map.contains(buf_id))
				{
					GfxResourceState prev_state = prev_dependency_level.buffer_state_map[buf_id];
					if (prev_state != state)
					{
						cmd_list->BufferBarrier(*buffer, prev_state, state);
					}
					found = true;
					break;
				}
			}
			if (!found && rg_buffer->imported)
			{
				if (GfxResourceState::Common != state)
				{
					cmd_list->BufferBarrier(*buffer, GfxResourceState::Common, state);
				}
			}
		}
		cmd_list->FlushBarriers();
	}

	void RenderGraph::DependencyLevel::PostExecute(GfxCommandList* cmd_list)
	{
		for (RGTextureId tex_id : texture_destroys)
		{
			RGTexture* rg_texture = rg.GetRGTexture(tex_id);
			GfxTexture* texture = rg_texture->resource;
			GfxResourceState initial_state = texture->GetDesc().initial_state;
			ADRIA_ASSERT(texture_state_map.contains(tex_id));
			GfxResourceState state = texture_state_map[tex_id];
			if (initial_state != state)
			{
				cmd_list->TextureBarrier(*texture, state, initial_state);
			}
			if (!rg_texture->imported)
			{
				rg.pool.ReleaseTexture(rg_texture->resource);
			}
		}
		for (RGBufferId buf_id : buffer_destroys)
		{
			RGBuffer* rg_buffer = rg.GetRGBuffer(buf_id);
			GfxBuffer* buffer = rg_buffer->resource;
			ADRIA_ASSERT(buffer_state_map.contains(buf_id));
			GfxResourceState state = buffer_state_map[buf_id];
			if (state != GfxResourceState::Common)
			{
				cmd_list->BufferBarrier(*buffer, state, GfxResourceState::Common);
			}
			if (!rg_buffer->imported)
			{
				rg.pool.ReleaseBuffer(rg_buffer->resource);
			}
		}
		cmd_list->FlushBarriers();
	}

	void RenderGraph::PushEvent(Char const* name)
	{
		if (g_UseDependencyLevels)
		{
			return;
		}
		pending_event_indices.push_back(AddEvent(name));
	}

	void RenderGraph::PopEvent()
	{
		if (g_UseDependencyLevels)
		{
			return;
		}

		if (!pending_event_indices.empty())
		{
			pending_event_indices.pop_back();
		}
		else
		{
			passes.back()->num_events_to_end++;
		}
	}

	void RenderGraph::Dump(Char const* graph_file_name)
	{
		static struct GraphVizStyle
		{
			Char const* rank_dir{ "TB" };
			struct
			{
				Char const* name{ "helvetica" };
				Int32       size{ 10 };
			} font;
			struct
			{
				struct
				{
					Char const* executed{ "orange" };
					Char const* culled{ "lightgray" };
				} pass;
				struct
				{
					Char const* imported{ "lightsteelblue" };
					Char const* transient{ "skyblue" };
				} resource;
				struct
				{
					Char const* read{ "olivedrab3" };
					Char const* write{ "orangered" };
				} edge;
			} color;
		} style;

		struct GraphViz
		{
			std::string defaults;
			std::string declarations;
			std::string dependencies;
		} graphviz;

		graphviz.defaults += std::format("graph [style=invis, rankdir=\"{}\", ordering=out, splines=spline]\n", style.rank_dir);
		graphviz.defaults += std::format("node [shape=record, fontname=\"{}\", fontsize={}, margin=\"0.2,0.03\"]\n", style.font.name, style.font.size);

		auto PairHash = [](std::pair<Uint64, Uint64> const& p)
			{
				return std::hash<Uint64>{}(p.first) + std::hash<Uint64>{}(p.second);
			};
		std::unordered_set<std::pair<Uint64, Uint64>, decltype(PairHash)> declared_buffers;
		std::unordered_set<std::pair<Uint64, Uint64>, decltype(PairHash)> declared_textures;
		auto DeclareBuffer = [&declared_buffers, &graphviz, this](RGBuffer* buffer)
			{
				auto decl_pair = std::make_pair(buffer->id, buffer->version);
				if (!declared_buffers.contains(decl_pair))
				{
					buffer->desc.size;
					graphviz.declarations += std::format("B{}_{} ", buffer->id, buffer->version);
					std::string label = std::format("<{}<br/>dimension: Buffer<br/>size: {} bytes <br/>format: {} <br/>version: {} <br/>refs: {}<br/>{}>",
						buffer->name, buffer->desc.size, GfxFormatToString(buffer->desc.format), buffer->version, buffer->ref_count, buffer->imported ? "Imported" : "Transient");
					graphviz.declarations += std::format("[shape=\"box\", style=\"filled\",fillcolor={}, label={}] \n", buffer->imported ? style.color.resource.imported : style.color.resource.transient, label);
					declared_buffers.insert(decl_pair);
				}
			};
		auto DeclareTexture = [&declared_textures, &graphviz, this](RGTexture* texture)
			{
				auto decl_pair = std::make_pair(texture->id, texture->version);
				if (!declared_textures.contains(decl_pair))
				{
					std::string dimensions;
					switch (texture->desc.type)
					{
					case GfxTextureType_1D:  dimensions += std::format("width = {}", texture->desc.width); break;
					case GfxTextureType_2D:  dimensions += std::format("width = {}, height = {}", texture->desc.width, texture->desc.height); break;
					case GfxTextureType_3D:  dimensions += std::format("width = {}, height = {}, depth = {}", texture->desc.width, texture->desc.height, texture->desc.depth); break;
					}

					if (texture->desc.array_size > 1)  dimensions += std::format(", array size = {}", texture->desc.array_size);

					graphviz.declarations += std::format("T{}_{} ", texture->id, texture->version);
					std::string label = std::format("<{} <br/>dimension: {}<br/>{}<br/>format: {} <br/>version: {} <br/>refs: {}<br/>{}>",
						texture->name, GfxTextureTypeToString(texture->desc.type), dimensions, GfxFormatToString(texture->desc.format), texture->version, texture->ref_count, texture->imported ? "Imported" : "Transient");
					graphviz.declarations += std::format("[shape=\"box\", style=\"filled\",fillcolor={}, label={}] \n", texture->imported ? style.color.resource.imported : style.color.resource.transient, label);
					declared_textures.insert(decl_pair);
				}
			};

		for (auto const& dependency_level : dependency_levels)
		{
			for (auto const& pass : dependency_level.passes)
			{
				graphviz.declarations += std::format("P{} ", pass->id);
				std::string label = std::format("<{}<br/> type: {}<br/> refs: {}<br/> culled: {}>", pass->name, RGPassTypeToString(pass->type), pass->ref_count, pass->IsCulled() ? "Yes" : "No");
				graphviz.declarations += std::format("[shape=\"ellipse\", style=\"rounded,filled\",fillcolor={}, label={}] \n",
					pass->IsCulled() ? style.color.pass.culled : style.color.pass.executed, label);

				std::string read_dependencies = "{";
				std::string write_dependencies = "{";

				for (auto const& buffer_read : pass->buffer_reads)
				{
					RGBuffer* buffer = GetRGBuffer(buffer_read);
					DeclareBuffer(buffer);
					read_dependencies += std::format("B{}_{},", buffer->id, buffer->version);
				}

				for (auto const& texture_read : pass->texture_reads)
				{
					RGTexture* texture = GetRGTexture(texture_read);
					DeclareTexture(texture);
					read_dependencies += std::format("T{}_{},", texture->id, texture->version);
				}

				for (auto const& buffer_write : pass->buffer_writes)
				{
					RGBuffer* buffer = GetRGBuffer(buffer_write);
					if (!pass->buffer_creates.contains(buffer_write)) buffer->version++;
					DeclareBuffer(buffer);
					write_dependencies += std::format("B{}_{},", buffer->id, buffer->version);
				}

				for (auto const& texture_write : pass->texture_writes)
				{
					RGTexture* texture = GetRGTexture(texture_write);
					if (!pass->texture_creates.contains(texture_write)) texture->version++;
					DeclareTexture(texture);
					write_dependencies += std::format("T{}_{},", texture->id, texture->version);
				}

				if (read_dependencies.back() == ',') read_dependencies.pop_back();
				read_dependencies += "}";
				if (write_dependencies.back() == ',') write_dependencies.pop_back();
				write_dependencies += "}";

				graphviz.dependencies += std::format("{}->P{} [color=olivedrab3]\n", read_dependencies, pass->id);
				graphviz.dependencies += std::format("P{}->{} [color=orangered]\n", pass->id, write_dependencies);
			}
		}

		std::string absolute_graph_path = paths::RenderGraphDir + graph_file_name;
		std::ofstream graph_file(absolute_graph_path);
		graph_file << "digraph RenderGraph{ \n";
		graph_file << graphviz.defaults << "\n";
		graph_file << graphviz.declarations << "\n";
		graph_file << graphviz.dependencies << "\n";
		graph_file << "}";
		graph_file.close();

		std::string filename = GetFilenameWithoutExtension(graph_file_name);
		std::string cmd = std::format("Tools\\graphviz\\dot.exe -Tsvg {} > {}{}.svg", absolute_graph_path, paths::RenderGraphDir, filename);
		system(cmd.c_str());
	}

	void RenderGraph::DumpDebugData()
	{
		std::string render_graph_data = "";
		render_graph_data += "Passes: \n";
		for (Uint64 i = 0; i < passes.size(); ++i)
		{
			auto& pass = passes[i];
			render_graph_data += std::format("Pass {}: {}\n", i, pass->name);
			render_graph_data += "\nTexture usage:\n";
			for (auto [tex_id, state] : pass->texture_state_map)
			{
				render_graph_data += std::format("Texture ID: {}, State: {}\n", tex_id.id, ConvertBarrierFlagsToString(state));
			}
			render_graph_data += "\nBuffer usage:\n";
			for (auto [buf_id, state] : pass->buffer_state_map)
			{
				render_graph_data += std::format("Buffer ID: {}, State: {}\n", buf_id.id, ConvertBarrierFlagsToString(state));
			}
			render_graph_data += "\n";
		}

		render_graph_data += "\nAdjacency lists: \n";
		for (Uint64 i = 0; i < adjacency_lists.size(); ++i)
		{
			auto& list = adjacency_lists[i];
			render_graph_data += std::format("{}. {}'s adjacency list: ", i, passes[i]->name);
			for (auto j : list) render_graph_data += std::format(" {} ", j);
			render_graph_data += "\n";
		}

		render_graph_data += "\nTopologically sorted passes: \n";
		for (Uint64 i = 0; i < topologically_sorted_passes.size(); ++i)
		{
			Uint64& topologically_sorted_pass = topologically_sorted_passes[i];
			render_graph_data += std::format("{}. : {}\n", i, passes[topologically_sorted_pass]->name);
		}

		render_graph_data += "\nDependency levels: \n";
		for (Uint64 i = 0; i < dependency_levels.size(); ++i)
		{
			auto& level = dependency_levels[i];
			render_graph_data += std::format("Dependency level {}: \n", i);
			for (auto pass : level.passes) render_graph_data += std::format("{}\n", pass->name);
			render_graph_data += "\nTexture usage:\n";
			for (auto [tex_id, state] : level.texture_state_map)
			{
				render_graph_data += std::format("Texture ID: {}, State: {}\n", tex_id.id, ConvertBarrierFlagsToString(state));
			}
			render_graph_data += "\nBuffer usage:\n";
			for (auto [buf_id, state] : level.buffer_state_map)
			{
				render_graph_data += std::format("Buffer ID: {}, State: {}\n", buf_id.id, ConvertBarrierFlagsToString(state));
			}
			render_graph_data += "\n";
		}
		render_graph_data += "\nTextures: \n";
		for (Uint64 i = 0; i < textures.size(); ++i)
		{
			auto& texture = textures[i];
			render_graph_data += std::format("Texture: id = {}, name = {}, last used by: {} \n", texture->id, texture->name, texture->last_used_by->name);
		}
		render_graph_data += "\nBuffers: \n";
		for (Uint64 i = 0; i < buffers.size(); ++i)
		{
			auto& buffer = buffers[i];
			render_graph_data += std::format("Buffer: id = {}, name = {}, last used by: {} \n", buffer->id, buffer->name, buffer->last_used_by->name);
		}
		ADRIA_LOG(DEBUG, "[RenderGraph]\n%s", render_graph_data.c_str());
	}
}

 