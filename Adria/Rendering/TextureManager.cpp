#include "d3dx12.h"
#include "TextureManager.h"
#include "Graphics/GfxTexture.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxCommon.h"
#include "Graphics/GfxCommandList.h"
#include "Graphics/GfxShaderCompiler.h"
#include "Utilities/Image.h"


namespace adria
{

    TextureManager::TextureManager() {}
    TextureManager::~TextureManager() = default;

	void TextureManager::Initialize(GfxDevice* _gfx)
	{
        gfx = _gfx;
	}

	void TextureManager::Clear()
	{
		for (auto& [handle, descriptor] : texture_srv_map)
		{
			gfx->FreeDescriptorCPU(descriptor, GfxDescriptorHeapType::CBV_SRV_UAV);
		}
		handle = TEXTURE_MANAGER_START_HANDLE;
		texture_srv_map.clear();
		texture_map.clear();
		loaded_textures.clear();
		is_scene_initialized = false;
	}

	void TextureManager::Shutdown()
	{
		Clear();
		gfx = nullptr;
	}

    TextureHandle TextureManager::LoadTexture(std::string_view path, Bool srgb)
    {
        std::string texture_name(path);
        if (auto it = loaded_textures.find(texture_name); it == loaded_textures.end())
        {
            ++handle;
            loaded_textures.insert({ texture_name, handle });
            Image img(path);

			GfxTextureDesc desc{};
			desc.type = img.Depth() > 1 ? GfxTextureType_3D : GfxTextureType_2D;
			desc.misc_flags = GfxTextureMiscFlag::None;
			desc.width = img.Width();
			desc.height = img.Height();
			desc.array_size = img.IsCubemap() ? 6 : 1;
			desc.depth = img.Depth();
			desc.bind_flags = GfxBindFlag::ShaderResource;
			desc.format = img.Format();
			desc.initial_state = GfxResourceState::AllSRV; 
			desc.heap_type = GfxResourceUsage::Default;
			desc.mip_levels = img.MipLevels();
            desc.misc_flags = img.IsCubemap() ? GfxTextureMiscFlag::TextureCube : GfxTextureMiscFlag::None;
			if (srgb)
			{
				desc.misc_flags |= GfxTextureMiscFlag::SRGB;
			}

            std::vector<GfxTextureSubData> tex_data;
			const Image* curr_img = &img;
			while (curr_img)
			{
				for (Uint32 i = 0; i < desc.mip_levels; ++i)
				{
                    GfxTextureSubData& data = tex_data.emplace_back();
					data.data = curr_img->MipData(i);
					data.row_pitch = GetRowPitch(curr_img->Format(), desc.width, i);
					data.slice_pitch = GetSlicePitch(img.Format(), desc.width, desc.height, i);
				}
                curr_img = curr_img->NextImage();
			}

			GfxTextureData init_data{};
			init_data.sub_data = tex_data.data();
			init_data.sub_count = (Uint32)tex_data.size();
            std::unique_ptr<GfxTexture> tex = gfx->CreateTexture(desc, init_data);

            texture_map[handle] = std::move(tex);
			CreateViewForTexture(handle);
			return handle;
        }
	    else return it->second;
    }

	TextureHandle TextureManager::LoadCubemap(std::array<std::string, 6> const& cubemap_textures)
	{
		++handle;
		GfxTextureDesc desc{};
		desc.type = GfxTextureType_2D;
		desc.mip_levels = 1;
		desc.misc_flags = GfxTextureMiscFlag::TextureCube;
		desc.array_size = 6;
		desc.bind_flags = GfxBindFlag::ShaderResource;

		std::vector<Image> images{};
		std::vector<GfxTextureSubData> subresources;
		for (Uint32 i = 0; i < cubemap_textures.size(); ++i)
		{
			images.emplace_back(cubemap_textures[i]);
			GfxTextureSubData subresource_data{};
			subresource_data.data = images.back().Data<void>();
			subresource_data.row_pitch = GetRowPitch(images.back().Format(), desc.width, 0);
			subresources.push_back(subresource_data);
		}
		desc.width  = images[0].Width();
		desc.height = images[0].Height();
		desc.format = images[0].IsHDR() ? GfxFormat::R32G32B32A32_FLOAT : GfxFormat::R8G8B8A8_UNORM;

		GfxTextureData init_data{};
		init_data.sub_data = subresources.data();
		std::unique_ptr<GfxTexture> cubemap = gfx->CreateTexture(desc, init_data);

		texture_map.insert({ handle, std::move(cubemap) });
		CreateViewForTexture(handle);
		return handle;
	}

	GfxDescriptor TextureManager::GetSRV(TextureHandle tex_handle)
	{
		return texture_srv_map[tex_handle];
	}

	GfxTexture* TextureManager::GetTexture(TextureHandle handle) const
	{
		if (handle == INVALID_TEXTURE_HANDLE)
		{
			return nullptr;
		}
		else if (auto it = texture_map.find(handle); it != texture_map.end())
		{
			return it->second.get();
		}
		return nullptr;
	}

	void TextureManager::EnableMipMaps(Bool mips)
    {
        enable_mipmaps = mips;
    }

	void TextureManager::OnSceneInitialized()
	{
		gfx->InitGlobalResourceBindings(1024);
		gfx->CopyDescriptors(1, gfx->GetDescriptorGPU((Uint32)DEFAULT_BLACK_TEXTURE_HANDLE), gfxcommon::GetCommonView(GfxCommonViewType::BlackTexture2D_SRV));
		gfx->CopyDescriptors(1, gfx->GetDescriptorGPU((Uint32)DEFAULT_WHITE_TEXTURE_HANDLE), gfxcommon::GetCommonView(GfxCommonViewType::WhiteTexture2D_SRV));
		gfx->CopyDescriptors(1, gfx->GetDescriptorGPU((Uint32)DEFAULT_NORMAL_TEXTURE_HANDLE), gfxcommon::GetCommonView(GfxCommonViewType::DefaultNormal2D_SRV));
		gfx->CopyDescriptors(1, gfx->GetDescriptorGPU((Uint32)DEFAULT_METALLIC_ROUGHNESS_TEXTURE_HANDLE), gfxcommon::GetCommonView(GfxCommonViewType::MetallicRoughness2D_SRV));
		for (Uint64 i = TEXTURE_MANAGER_START_HANDLE; i <= handle; ++i)
        {
            GfxTexture* texture = texture_map[TextureHandle(i)].get();
            if (texture)
            {
                CreateViewForTexture(TextureHandle(i), true);
            }
        }
        is_scene_initialized = true;
	}

	void TextureManager::CreateViewForTexture(TextureHandle handle, Bool flag)
	{
        if (!is_scene_initialized && !flag) return;

		GfxTexture* texture = texture_map[handle].get();
		ADRIA_ASSERT(texture);
        texture_srv_map[handle] = gfx->CreateTextureSRV(texture);
        gfx->CopyDescriptors(1, gfx->GetDescriptorGPU((Uint32)handle), texture_srv_map[handle]);
	}

}