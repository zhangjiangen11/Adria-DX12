#include "ShadowRenderer.h"
#include "Components.h"
#include "Camera.h"
#include "ShaderManager.h"
#include "BlackboardData.h"
#include "ShaderStructs.h"
#include "Graphics/GfxBuffer.h"
#include "Graphics/GfxBufferView.h"
#include "Graphics/GfxTexture.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxCommandList.h"
#include "Graphics/GfxShaderCompiler.h"
#include "Graphics/GfxPipelineStatePermutations.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"
#include "Core/ConsoleManager.h"

using namespace DirectX;

namespace adria
{
	static TAutoConsoleVariable<Float> CascadesSplitLambda("r.Shadows.CascadesSplitLambda", 0.5f, "Lambda used when calculating cascades split");
	static TAutoConsoleVariable<Float> ShadowFarFactor("r.Shadows.FarFactor", 1.2f, "Far factor used to calculate projection matrices of directional light");
	static TAutoConsoleVariable<Float> ShadowLightDistanceFactor("r.Shadows.LightDistanceFactor", 1.0f, "Factor used to calculate projection matrices of directional light");

	namespace
	{
		std::pair<Matrix, Matrix> LightViewProjection_Directional(Light const& light, Camera const& camera, Uint32 shadow_size, std::vector<BoundingObject>& bounding_objects)
		{
			BoundingFrustum frustum = camera.Frustum();
			std::array<Vector3, BoundingFrustum::CORNER_COUNT> corners = {};
			frustum.GetCorners(corners.data());

			BoundingSphere frustum_sphere;
			BoundingSphere::CreateFromFrustum(frustum_sphere, frustum);

			Vector3 frustum_center(0, 0, 0);
			for (Uint32 i = 0; i < corners.size(); ++i)
			{
				frustum_center = frustum_center + corners[i];
			}
			frustum_center /= static_cast<Float>(corners.size());

			Float radius = 0.0f;
			for (Vector3 const& corner : corners)
			{
				Float distance = Vector3::Distance(corner, frustum_center);
				radius = std::max(radius, distance);
			}
			radius = std::ceil(radius * 8.0f) / 8.0f;

			Vector3 const max_extents(radius, radius, radius);
			Vector3 const min_extents = -max_extents;

			Vector3 light_dir = XMVector3Normalize(light.direction);
			Matrix V = XMMatrixLookAtLH(frustum_center, frustum_center + 1.0f * light_dir * radius, Vector3::Up);

			Float l = min_extents.x;
			Float b = min_extents.y;
			Float n = min_extents.z;
			Float r = max_extents.x;
			Float t = max_extents.y;
			Float f = max_extents.z * 1.5f;

			Matrix P = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);
			Matrix VP = V * P;
			Vector3 shadow_origin(0, 0, 0);
			shadow_origin = Vector3::Transform(shadow_origin, VP);
			shadow_origin *= (shadow_size / 2.0f);
			
			Vector3 rounded_origin = XMVectorRound(shadow_origin);
			Vector3 rounded_offset = rounded_origin - shadow_origin;
			rounded_offset *= (2.0f / shadow_size);
			rounded_offset.z = 0.0f;
			P.m[3][0] += rounded_offset.x;
			P.m[3][1] += rounded_offset.y;

			BoundingBox box;
			BoundingBox::CreateFromPoints(box, Vector4(l, b, n, 1.0f), Vector4(r, t, f, 1.0f));
			box.Transform(box, V.Invert());
			bounding_objects.emplace_back(box);

			return { V,P };
		}
		std::pair<Matrix, Matrix> LightViewProjection_Spot(Light const& light, std::vector<BoundingObject>& bounding_objects)
		{
			ADRIA_ASSERT(light.type == LightType::Spot);

			Vector3 light_dir = XMVector3Normalize(light.direction);
			Vector3 light_pos = Vector3(light.position);
			Vector3 target_pos = light_pos + light_dir * light.range;

			Matrix V = XMMatrixLookAtLH(light_pos, target_pos, Vector3::Up);
			static const Float shadow_near = 0.5f;
			Float fov_angle = 2.0f * acos(light.outer_cosine);
			Matrix P = XMMatrixPerspectiveFovLH(fov_angle, 1.0f, shadow_near, light.range);

			BoundingFrustum frustum(P);
			frustum.Transform(frustum, V.Invert());
			bounding_objects.emplace_back(frustum);

			return { V,P };
		}
		std::pair<Matrix, Matrix> LightViewProjection_Point(Light const& light, Uint32 face_index, std::vector<BoundingObject>& bounding_objects)
		{
			static Float const shadow_near = 0.5f;
			Matrix P = XMMatrixPerspectiveFovLH(XMConvertToRadians(90.0f), 1.0f, shadow_near, light.range);

			Vector3 light_pos = Vector3(light.position);
			Matrix V{};
			Vector3 target{};
			Vector3 up{};
			switch (face_index)
			{
			case 0:
				target = light_pos + Vector3(1, 0, 0); 
				up = Vector3(0.0f, 1.0f, 0.0f);
				V = XMMatrixLookAtLH(light_pos, target, up);
				break;
			case 1:
				target = light_pos + Vector3(-1, 0, 0);
				up = Vector3(0.0f, 1.0f, 0.0f);
				V = XMMatrixLookAtLH(light_pos, target, up);
				break;
			case 2:
				target = light_pos + Vector3(0, 1, 0);
				up = Vector3(0.0f, 0.0f, -1.0f);
				V = XMMatrixLookAtLH(light_pos, target, up);
				break;
			case 3:
				target = light_pos + Vector3(0, -1, 0);
				up = Vector3(0.0f, 0.0f, 1.0f);
				V = XMMatrixLookAtLH(light_pos, target, up);
				break;
			case 4:
				target = light_pos + Vector3(0, 0, 1);
				up = Vector3(0.0f, 1.0f, 0.0f);
				V = XMMatrixLookAtLH(light_pos, target, up);
				break;
			case 5:
				target = light_pos + Vector3(0, 0, -1);
				up = Vector3(0.0f, 1.0f, 0.0f);
				V = XMMatrixLookAtLH(light_pos, target, up);
				break;
			default:
				ADRIA_ASSERT_MSG(false, "Invalid face index!");
			}

			BoundingFrustum frustum(P);
			frustum.Transform(frustum, V.Invert());
			bounding_objects.emplace_back(frustum);

			return { V,P };
		}
		std::pair<Matrix, Matrix> LightViewProjection_Cascades(Light const& light, Camera const& camera, Matrix const& projection_matrix, Uint32 shadow_cascade_size, std::vector<BoundingObject>& bounding_objects)
		{
			Float const far_factor = ShadowFarFactor.Get();
			Float const light_distance_factor = ShadowLightDistanceFactor.Get();

			BoundingFrustum frustum(projection_matrix);
			frustum.Transform(frustum, camera.View().Invert());
			std::array<Vector3, BoundingFrustum::CORNER_COUNT> corners{};
			frustum.GetCorners(corners.data());

			Vector3 frustum_center(0, 0, 0);
			for (Vector3 const& corner : corners)
			{
				frustum_center = frustum_center + corner;
			}
			frustum_center /= static_cast<Float>(corners.size());

			Float radius = 0.0f;
			for (Vector3 const& corner : corners)
			{
				Float distance = Vector3::Distance(corner, frustum_center);
				radius = std::max(radius, distance);
			}
			radius = std::ceil(radius * 8.0f) / 8.0f;

			Vector3 const max_extents(radius, radius, radius);
			Vector3 const min_extents = -max_extents;
			Vector3 const cascade_extents = max_extents - min_extents;

			Vector3 light_dir = XMVector3Normalize(light.direction);
			Matrix V = XMMatrixLookAtLH(frustum_center, frustum_center + light_distance_factor * light_dir * radius, Vector3::Up);

			Float l = min_extents.x;
			Float b = min_extents.y;
			Float n = min_extents.z - far_factor * radius;
			Float r = max_extents.x;
			Float t = max_extents.y;
			Float f = max_extents.z * far_factor;

			Matrix P = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);
			Matrix VP = V * P;
			Vector3 shadow_origin(0, 0, 0);
			shadow_origin = Vector3::Transform(shadow_origin, VP);
			shadow_origin *= (shadow_cascade_size / 2.0f);

			Vector3 rounded_origin = XMVectorRound(shadow_origin);
			Vector3 rounded_offset = rounded_origin - shadow_origin;
			rounded_offset *= (2.0f / shadow_cascade_size);
			rounded_offset.z = 0.0f;
			P.m[3][0] += rounded_offset.x;
			P.m[3][1] += rounded_offset.y;

			BoundingBox box;
			BoundingBox::CreateFromPoints(box, Vector4(l, b, n, 1.0f), Vector4(r, t, f, 1.0f));
			box.Transform(box, V.Invert());
			bounding_objects.emplace_back(box);

			return { V,P };
		}
	}

	ShadowRenderer::ShadowRenderer(entt::registry& reg, GfxDevice* gfx, Uint32 width, Uint32 height) : reg(reg), gfx(gfx), width(width), height(height),
		ray_traced_shadows_pass(gfx, width, height) 
	{
		CreatePSOs();
	}
	ShadowRenderer::~ShadowRenderer() {}

	void ShadowRenderer::FillFrameCBuffer(FrameCBuffer& frame_cbuffer)
	{
		frame_cbuffer.lights_matrices_idx = light_matrices_gpu_index;
		frame_cbuffer.cascade_splits = Vector4(split_distances[0], split_distances[1], split_distances[2], split_distances[3]);
	}

	void ShadowRenderer::GUI()
	{
		QueueGUI([&]()
			{
				if (ImGui::TreeNodeEx("Shadow Settings", ImGuiTreeNodeFlags_None))
				{
					ImGui::SliderFloat("Cascades Split Lambda", CascadesSplitLambda.GetPtr(), 0.0f, 1.0f);
					ImGui::SliderFloat("Far Plane Factor", ShadowFarFactor.GetPtr(), 0.1f, 4.0f);

					ImGui::TreePop();
					ImGui::Separator();
				}
			}, GUICommandGroup_Renderer
		);
	}

	void ShadowRenderer::SetupShadows(Camera const* camera)
	{
		static constexpr Uint32 backbuffer_count = GFX_BACKBUFFER_COUNT;
		Uint32 backbuffer_index = gfx->GetBackbufferIndex();

		auto AddShadowMask = [&](Light& light, Uint64 light_id)
		{
			if (light_mask_textures[light_id] == nullptr)
			{
				GfxTextureDesc mask_desc{};
				mask_desc.width = width;
				mask_desc.height = height;
				mask_desc.format = GfxFormat::R8_UNORM;
				mask_desc.initial_state = GfxResourceState::ComputeUAV;
				mask_desc.bind_flags = GfxBindFlag::UnorderedAccess | GfxBindFlag::UnorderedAccess;

				light_mask_textures[light_id] = gfx->CreateTexture(mask_desc);

				light_mask_texture_srvs[light_id] = gfx->CreateTextureSRV(light_mask_textures[light_id].get());
				light_mask_texture_uavs[light_id] = gfx->CreateTextureUAV(light_mask_textures[light_id].get());
			}
			GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(light_mask_texture_srvs[light_id]);
			light.shadow_mask_index = static_cast<Int32>(table);
		};
		auto AddShadowMap  = [&](Uint64 light_id, Uint32 shadow_map_size)
		{
			GfxTextureDesc depth_desc{};
			depth_desc.width = shadow_map_size;
			depth_desc.height = shadow_map_size;
			depth_desc.format = GfxFormat::R32_TYPELESS;
			depth_desc.clear_value = GfxClearValue(1.0f, 0);
			depth_desc.bind_flags = GfxBindFlag::DepthStencil | GfxBindFlag::ShaderResource;
			depth_desc.initial_state = GfxResourceState::DSV;

			light_shadow_maps[light_id].emplace_back(gfx->CreateTexture(depth_desc));
			light_shadow_map_srvs[light_id].push_back(gfx->CreateTextureSRV(light_shadow_maps[light_id].back().get()));
			light_shadow_map_dsvs[light_id].push_back(gfx->CreateTextureDSV(light_shadow_maps[light_id].back().get()));
		};
		auto AddShadowMaps = [&](Light& light, Uint64 light_id)
		{
			switch (light.type)
			{
			case LightType::Directional:
			{
				if (light.use_cascades && light_shadow_maps[light_id].size() != SHADOW_CASCADE_COUNT)
				{
					light_shadow_maps[light_id].clear();
					light_shadow_map_srvs[light_id].clear();
					light_shadow_map_dsvs[light_id].clear();
					for (Uint32 i = 0; i < SHADOW_CASCADE_COUNT; ++i) AddShadowMap(light_id, SHADOW_CASCADE_MAP_SIZE);
				}
				else if (!light.use_cascades && light_shadow_maps[light_id].size() != 1)
				{
					light_shadow_maps[light_id].clear();
					light_shadow_map_srvs[light_id].clear();
					light_shadow_map_dsvs[light_id].clear();
					AddShadowMap(light_id, SHADOW_MAP_SIZE);
				}
			}
			break;
			case LightType::Point:
			{
				if (light_shadow_maps[light_id].size() != 6)
				{
					light_shadow_maps[light_id].clear();
					light_shadow_map_srvs[light_id].clear();
					light_shadow_map_dsvs[light_id].clear();
					for (Uint32 i = 0; i < 6; ++i) AddShadowMap(light_id, SHADOW_CUBE_SIZE);
				}
			}
			break;
			case LightType::Spot:
			{
				if (light_shadow_maps[light_id].size() != 1)
				{
					light_shadow_maps[light_id].clear();
					light_shadow_map_srvs[light_id].clear();
					light_shadow_map_dsvs[light_id].clear();
					AddShadowMap(light_id, SHADOW_MAP_SIZE);
				}
			}
			break;
			}

			GfxBindlessTable table = gfx->AllocateBindlessTable(light_shadow_maps[light_id].size());
			for (Uint64 j = 0; j < light_shadow_maps[light_id].size(); ++j)
			{
				gfx->UpdateBindlessTable(table, j, light_shadow_map_srvs[light_id][j]);
				if (j == 0)
				{
					light.shadow_texture_index = table;
				}
			}
		};

		static Uint64 light_matrices_count = 0;
		Uint64 current_light_matrices_count = 0;

		auto light_view = reg.view<Light>();
		for (entt::entity e : light_view)
		{
			Light& light = light_view.get<Light>(e);
			if (light.casts_shadows)
			{
				if (light.type == LightType::Directional && light.use_cascades) current_light_matrices_count += SHADOW_CASCADE_COUNT;
				else if (light.type == LightType::Point) current_light_matrices_count += 6;
				else current_light_matrices_count++;
			}
		}

		if (current_light_matrices_count != light_matrices_count)
		{
			gfx->WaitForGPU();
			light_matrices_count = current_light_matrices_count;
			if (light_matrices_count != 0)
			{
				light_matrices_buffer = gfx->CreateBuffer(StructuredBufferDesc<Matrix>(light_matrices_count * backbuffer_count, false, true));
				GfxBufferDescriptorDesc srv_desc{};
				srv_desc.size = light_matrices_count * sizeof(Matrix);
				for (Uint32 i = 0; i < backbuffer_count; ++i)
				{
					srv_desc.offset = i * light_matrices_count * sizeof(Matrix);
					light_matrices_buffer_srvs[i] = gfx->CreateBufferSRV(light_matrices_buffer.get(), &srv_desc);
				}
			}
		}

		bounding_objects.clear();
		std::vector<Matrix> light_matrices;
		light_matrices.reserve(light_matrices_count);
		for (entt::entity e : light_view)
		{
			Light& light = light_view.get<Light>(e);
			light.shadow_mask_index = -1;
			light.shadow_texture_index = -1;
			if (light.casts_shadows)
			{
				if (light.ray_traced_shadows) continue;
				light.shadow_matrix_index = (Uint32)light_matrices.size();
				if (light.type == LightType::Directional)
				{
					if (light.use_cascades)
					{
						std::array<Matrix, SHADOW_CASCADE_COUNT> proj_matrices = RecalculateProjectionMatrices(*camera, CascadesSplitLambda.Get(), split_distances);
						AddShadowMaps(light, entt::to_integral(e));
						for (Uint32 i = 0; i < SHADOW_CASCADE_COUNT; ++i)
						{
							auto const& [V, P] = LightViewProjection_Cascades(light, *camera, proj_matrices[i], SHADOW_CASCADE_MAP_SIZE, bounding_objects);
							light_matrices.push_back(XMMatrixTranspose(V * P));
						}
					}
					else
					{
						AddShadowMaps(light, entt::to_integral(e));
						auto const& [V, P] = LightViewProjection_Directional(light, *camera, SHADOW_MAP_SIZE, bounding_objects);
						light_matrices.push_back(XMMatrixTranspose(V * P));
					}

				}
				else if (light.type == LightType::Point)
				{
					AddShadowMaps(light, entt::to_integral(e));
					for (Uint32 i = 0; i < 6; ++i)
					{
						auto const& [V, P] = LightViewProjection_Point(light, i, bounding_objects);
						light_matrices.push_back(XMMatrixTranspose(V * P));
					}
				}
				else if (light.type == LightType::Spot)
				{
					AddShadowMaps(light, entt::to_integral(e));
					auto const& [V, P] = LightViewProjection_Spot(light, bounding_objects);
					light_matrices.push_back(XMMatrixTranspose(V * P));
				}
			}
			else if (light.ray_traced_shadows)
			{
				AddShadowMask(light, entt::to_integral(e));
			}
		}
		ADRIA_ASSERT(light_matrices.size() == bounding_objects.size());

		if (light_matrices_buffer)
		{
			light_matrices_buffer->Update(light_matrices.data(), light_matrices_count * sizeof(Matrix), light_matrices_count * sizeof(Matrix) * backbuffer_index);
			GfxBindlessTable table = gfx->AllocateAndUpdateBindlessTable(light_matrices_buffer_srvs[backbuffer_index]);
			light_matrices_gpu_index = static_cast<Int32>(table);
		}
	}

	void ShadowRenderer::AddShadowMapPasses(RenderGraph& rg)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();
		auto light_view = reg.view<Light>();
		for (entt::entity e : light_view)
		{
			Light& light = light_view.get<Light>(e);
			if (!light.casts_shadows) continue;
			Int32 light_index = light.light_index;
			Int32 light_matrix_index = light.shadow_matrix_index;
			Uint64 light_id = entt::to_integral(e);

			if (light.type == LightType::Directional)
			{
				if (light.use_cascades)
				{
					for (Uint32 i = 0; i < SHADOW_CASCADE_COUNT; ++i)
					{
						rg.ImportTexture(RG_NAME_IDX(ShadowMap, light_matrix_index + i), light_shadow_maps[light_id][i].get());
						std::string name = "Cascade Shadow Pass" + std::to_string(i);
						rg.AddPass<void>(name.c_str(),
							[=](RenderGraphBuilder& builder)
							{
								builder.WriteDepthStencil(RG_NAME_IDX(ShadowMap, light_matrix_index + i), RGLoadStoreAccessOp::Clear_Preserve);
								builder.SetViewport(SHADOW_CASCADE_MAP_SIZE, SHADOW_CASCADE_MAP_SIZE);
							},
							[=](RenderGraphContext& context)
							{
								GfxCommandList* cmd_list = context.GetCommandList();
								cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
								ShadowMapPass_Common(cmd_list, light.type, light_index, light_matrix_index, i);
							}, RGPassType::Graphics);

						shadow_rendered_event.Broadcast(RG_NAME_IDX(ShadowMap, light_matrix_index + i));
					}
				}
				else
				{
					rg.ImportTexture(RG_NAME_IDX(ShadowMap, light.shadow_matrix_index), light_shadow_maps[light_id][0].get());
					std::string name = "Directional Shadow Pass";
					rg.AddPass<void>(name.c_str(),
						[=](RenderGraphBuilder& builder)
						{
							builder.WriteDepthStencil(RG_NAME_IDX(ShadowMap, light.shadow_matrix_index), RGLoadStoreAccessOp::Clear_Preserve);
							builder.SetViewport(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
						},
						[=](RenderGraphContext& context)
						{
							GfxCommandList* cmd_list = context.GetCommandList();
							cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
							ShadowMapPass_Common(cmd_list, light.type, light_index, light_matrix_index, 0);
						}, RGPassType::Graphics);

					shadow_rendered_event.Broadcast(RG_NAME_IDX(ShadowMap, light.shadow_matrix_index));
				}
			}
			else if (light.type == LightType::Point)
			{
				for (Uint32 i = 0; i < 6; ++i)
				{
					rg.ImportTexture(RG_NAME_IDX(ShadowMap, light.shadow_matrix_index + i), light_shadow_maps[light_id][i].get());
					std::string name = "Point Shadow Pass" + std::to_string(i);
					rg.AddPass<void>(name.c_str(),
						[=](RenderGraphBuilder& builder)
						{
							builder.WriteDepthStencil(RG_NAME_IDX(ShadowMap, light.shadow_matrix_index + i), RGLoadStoreAccessOp::Clear_Preserve);
							builder.SetViewport(SHADOW_CUBE_SIZE, SHADOW_CUBE_SIZE);
						},
						[=](RenderGraphContext& context)
						{
							GfxCommandList* cmd_list = context.GetCommandList();
							cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
							ShadowMapPass_Common(cmd_list, light.type, light_index, light_matrix_index, i);
						}, RGPassType::Graphics);

					shadow_rendered_event.Broadcast(RG_NAME_IDX(ShadowMap, light.shadow_matrix_index + i));
				}
			}
			else if (light.type == LightType::Spot)
			{
				rg.ImportTexture(RG_NAME_IDX(ShadowMap, light_matrix_index), light_shadow_maps[light_id][0].get());
				std::string name = "Spot Shadow Pass";
				rg.AddPass<void>(name.c_str(),
					[=](RenderGraphBuilder& builder)
					{
						builder.WriteDepthStencil(RG_NAME_IDX(ShadowMap, light_matrix_index), RGLoadStoreAccessOp::Clear_Preserve);
						builder.SetViewport(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
					},
					[=](RenderGraphContext& context)
					{
						GfxCommandList* cmd_list = context.GetCommandList();
						cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
						ShadowMapPass_Common(cmd_list, light.type, light_index, light_matrix_index, 0);
					}, RGPassType::Graphics);

				shadow_rendered_event.Broadcast(RG_NAME_IDX(ShadowMap, light_matrix_index));
			}
		}
	}
	void ShadowRenderer::AddRayTracingShadowPasses(RenderGraph& rg)
	{
		auto light_view = reg.view<Light>();
		for (entt::entity e : light_view)
		{
			Light& light = light_view.get<Light>(e);
			if (!light.ray_traced_shadows) continue;
			Int32 light_index = light.light_index;
			Uint64 light_id = entt::to_integral(e);

			rg.ImportTexture(RG_NAME_IDX(LightMask, light_id), light_mask_textures[light_id].get());
			ray_traced_shadows_pass.AddPass(rg, light_index);
			shadow_rendered_event.Broadcast(RG_NAME_IDX(LightMask, light_id));
		}
	}

	void ShadowRenderer::CreatePSOs()
	{
		using enum GfxShaderStage;
		GfxGraphicsPipelineStateDesc gfx_pso_desc{};
		GfxShaderCompiler::FillInputLayoutDesc(SM_GetGfxShader(VS_Shadow), gfx_pso_desc.input_layout);
		gfx_pso_desc.root_signature = GfxRootSignatureID::Common;
		gfx_pso_desc.VS = VS_Shadow;
		gfx_pso_desc.PS = PS_Shadow;
		gfx_pso_desc.rasterizer_state.cull_mode = GfxCullMode::Front;
		gfx_pso_desc.rasterizer_state.fill_mode = GfxFillMode::Solid;
		gfx_pso_desc.rasterizer_state.depth_bias = 7500;
		gfx_pso_desc.rasterizer_state.depth_bias_clamp = 0.0f;
		gfx_pso_desc.rasterizer_state.slope_scaled_depth_bias = 1.0f;
		gfx_pso_desc.depth_state.depth_enable = true;
		gfx_pso_desc.depth_state.depth_write_mask = GfxDepthWriteMask::All;
		gfx_pso_desc.depth_state.depth_func = GfxComparisonFunc::LessEqual;
		gfx_pso_desc.dsv_format = GfxFormat::D32_FLOAT;

		shadow_psos = std::make_unique<GfxGraphicsPipelineStatePermutations>(gfx, gfx_pso_desc);
	}

	void ShadowRenderer::ShadowMapPass_Common(GfxCommandList* cmd_list, LightType light_type, Uint64 light_index, Uint64 matrix_index, Uint64 matrix_offset)
	{
		struct ShadowConstants
		{
			Uint32  light_index;
			Uint32  matrix_offset;
		} constants =
		{
			.light_index = (Uint32)light_index,
			.matrix_offset = (Uint32)matrix_offset
		};
		auto view = reg.view<Batch>();
		std::vector<Batch*> masked_batches, opaque_batches;
		for (entt::entity batch_entity : reg.view<Batch>())
		{
			Batch& batch = reg.get<Batch>(batch_entity);
			if (batch.alpha_mode == MaterialAlphaMode::Opaque) opaque_batches.push_back(&batch);
			else masked_batches.push_back(&batch);
		}

		auto DrawBatch = [&](GfxCommandList* cmd_list, Bool masked_batch)
		{
			std::vector<Batch*>& batches = masked_batch ? masked_batches : opaque_batches;
			if (masked_batch)
			{
				shadow_psos->AddDefine("TRANSPARENT", "1");
			}
			GfxPipelineState const* pso = shadow_psos->Get();
			cmd_list->SetRootConstants(1, constants);
			cmd_list->SetPipelineState(pso);
			for (Batch* batch : batches)
			{
				Bool skip_batch = false;
				switch (light_type)
				{
				case LightType::Directional:
					ADRIA_ASSERT(bounding_objects[matrix_index].type == BoundingObject::Box);
					skip_batch = !bounding_objects[matrix_index].GetBox().Intersects(batch->bounding_box);
					break;
				case LightType::Spot:
				case LightType::Point:
					ADRIA_ASSERT(bounding_objects[matrix_index].type == BoundingObject::Frustum);
					skip_batch = !bounding_objects[matrix_index].GetFrustum().Intersects(batch->bounding_box);
					break;
				default:
					ADRIA_ASSERT(false);
				}
				if (skip_batch) continue;

				struct ModelConstants
				{
					Uint32 instance_id;
				} model_constants{ .instance_id = batch->instance_id };
				cmd_list->SetRootCBV(2, model_constants);
				GfxIndexBufferView ibv(batch->submesh->buffer_address + batch->submesh->indices_offset, batch->submesh->indices_count);
				cmd_list->SetPrimitiveTopology(batch->submesh->topology);
				cmd_list->SetIndexBuffer(&ibv);
				cmd_list->DrawIndexed(batch->submesh->indices_count);
			}
		};

		DrawBatch(cmd_list, false);
		DrawBatch(cmd_list, true);
	}
	std::array<Matrix, ShadowRenderer::SHADOW_CASCADE_COUNT> ShadowRenderer::RecalculateProjectionMatrices(Camera const& camera, Float split_lambda, std::array<Float, SHADOW_CASCADE_COUNT>& split_distances)
	{
		Float camera_near = camera.Near();
		Float camera_far = camera.Far();
		Float near_plane = std::min(camera_near, camera_far);
		Float far_plane  = std::max(camera_near, camera_far);

		Float fov = camera.Fov();
		Float ar = camera.AspectRatio();

		Float f = 1.0f / SHADOW_CASCADE_COUNT;
		for (Uint32 i = 0; i < split_distances.size(); i++)
		{
			Float fi = (i + 1) * f;
			Float l = near_plane * pow(far_plane / near_plane, fi);
			Float u = near_plane + (far_plane - near_plane) * fi;
			split_distances[i] = l * split_lambda + u * (1.0f - split_lambda);
		}

		std::array<Matrix, SHADOW_CASCADE_COUNT> projection_matrices{};
		projection_matrices[0] = XMMatrixPerspectiveFovLH(fov, ar, near_plane, split_distances[0]);
		for (Uint32 i = 1; i < projection_matrices.size(); ++i)
			projection_matrices[i] = XMMatrixPerspectiveFovLH(fov, ar, split_distances[i - 1], split_distances[i]);
		return projection_matrices;
	}
}

