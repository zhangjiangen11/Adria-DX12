#include "D3D12Texture.h"
#include "D3D12Conversions.h"
#include "Graphics/GfxTexture.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxBuffer.h"
#include "Graphics/GfxCommandList.h"
#include "Graphics/GfxLinearDynamicAllocator.h"
#include "d3dx12.h"

namespace adria
{

	D3D12Texture::D3D12Texture(GfxDevice* gfx, GfxTextureDesc const& desc, GfxTextureData const& data) : GfxTexture(gfx, desc)
	{
		HRESULT hr = E_FAIL;
		D3D12MA::ALLOCATION_DESC allocation_desc{};
		allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC resource_desc{};
		resource_desc.Format = ConvertGfxFormat(desc.format);
		resource_desc.Width = desc.width;
		resource_desc.Height = desc.height;
		resource_desc.MipLevels = desc.mip_levels;
		resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		resource_desc.DepthOrArraySize = (Uint16)desc.array_size;
		resource_desc.SampleDesc.Count = desc.sample_count;
		resource_desc.SampleDesc.Quality = 0;
		resource_desc.Alignment = 0;
		resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		if (HasFlag(desc.bind_flags, GfxBindFlag::DepthStencil))
		{
			resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			if (!HasFlag(desc.bind_flags, GfxBindFlag::ShaderResource))
			{
				resource_desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
			}
		}
		if (HasFlag(desc.bind_flags, GfxBindFlag::RenderTarget))
		{
			resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		}
		if (HasFlag(desc.bind_flags, GfxBindFlag::UnorderedAccess))
		{
			resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		}

		switch (desc.type)
		{
		case GfxTextureType_1D:
			resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
			break;
		case GfxTextureType_2D:
			resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			break;
		case GfxTextureType_3D:
			resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
			resource_desc.DepthOrArraySize = (Uint16)desc.depth;
			break;
		default:
			ADRIA_ASSERT_MSG(false, "Invalid Texture Type!");
			break;
		}
		D3D12_CLEAR_VALUE* clear_value_ptr = nullptr;
		D3D12_CLEAR_VALUE clear_value{};
		if (HasFlag(desc.bind_flags, GfxBindFlag::DepthStencil) && desc.clear_value.active_member == GfxClearValue::GfxActiveMember::DepthStencil)
		{
			clear_value.DepthStencil.Depth = desc.clear_value.depth_stencil.depth;
			clear_value.DepthStencil.Stencil = desc.clear_value.depth_stencil.stencil;
			switch (desc.format)
			{
			case GfxFormat::R16_TYPELESS:
				clear_value.Format = DXGI_FORMAT_D16_UNORM;
				break;
			case GfxFormat::R32_TYPELESS:
				clear_value.Format = DXGI_FORMAT_D32_FLOAT;
				break;
			case GfxFormat::R24G8_TYPELESS:
				clear_value.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				break;
			case GfxFormat::R32G8X24_TYPELESS:
				clear_value.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
				break;
			default:
				clear_value.Format = ConvertGfxFormat(desc.format);
				break;
			}
			clear_value_ptr = &clear_value;
		}
		else if (HasFlag(desc.bind_flags, GfxBindFlag::RenderTarget) && desc.clear_value.active_member == GfxClearValue::GfxActiveMember::Color)
		{
			clear_value.Color[0] = desc.clear_value.color.color[0];
			clear_value.Color[1] = desc.clear_value.color.color[1];
			clear_value.Color[2] = desc.clear_value.color.color[2];
			clear_value.Color[3] = desc.clear_value.color.color[3];
			switch (desc.format)
			{
			case GfxFormat::R16_TYPELESS:
				clear_value.Format = DXGI_FORMAT_R16_UNORM;
				break;
			case GfxFormat::R32_TYPELESS:
				clear_value.Format = DXGI_FORMAT_R32_FLOAT;
				break;
			case GfxFormat::R24G8_TYPELESS:
				clear_value.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				break;
			case GfxFormat::R32G8X24_TYPELESS:
				clear_value.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
				break;
			default:
				clear_value.Format = ConvertGfxFormat(desc.format);
				break;
			}
			clear_value_ptr = &clear_value;
		}

		GfxResourceState initial_state = desc.initial_state;
		if (data.sub_data != nullptr)
		{
			initial_state = GfxResourceState::CopyDst;
		}

		ID3D12Device* device = gfx->GetDevice();
		if (desc.heap_type == GfxResourceUsage::Readback || desc.heap_type == GfxResourceUsage::Upload)
		{
			Uint64 required_size = 0;
			device->GetCopyableFootprints(&resource_desc, 0, 1, 0, nullptr, nullptr, nullptr, &required_size);
			resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			resource_desc.Width = required_size;
			resource_desc.Height = 1;
			resource_desc.DepthOrArraySize = 1;
			resource_desc.MipLevels = 1;
			resource_desc.Format = DXGI_FORMAT_UNKNOWN;
			resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

			if (desc.heap_type == GfxResourceUsage::Readback)
			{
				allocation_desc.HeapType = D3D12_HEAP_TYPE_READBACK;
				initial_state = GfxResourceState::CopyDst;
			}
			else if (desc.heap_type == GfxResourceUsage::Upload)
			{
				allocation_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
				initial_state = GfxResourceState::GenericRead;
			}

			if (HasFlag(desc.misc_flags, GfxTextureMiscFlag::Shared))
			{
				resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
				allocation_desc.ExtraHeapFlags |= D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
			}
		}
		D3D12MA::Allocator* allocator = gfx->GetAllocator();
		D3D12MA::Allocation* alloc = nullptr;
		if (gfx->GetCapabilities().SupportsEnhancedBarriers())
		{
			D3D12_RESOURCE_DESC1 resource_desc1 = CD3DX12_RESOURCE_DESC1(resource_desc);
			hr = allocator->CreateResource3(
				&allocation_desc,
				&resource_desc1,
				ToD3D12BarrierLayout(initial_state),
				clear_value_ptr, 0, nullptr,
				&alloc,
				IID_PPV_ARGS(resource.GetAddressOf())
			);
		}
		else
		{
			hr = allocator->CreateResource(
				&allocation_desc,
				&resource_desc,
				ToD3D12LegacyResourceState(initial_state),
				clear_value_ptr,
				&alloc,
				IID_PPV_ARGS(resource.GetAddressOf())
			);
		}
		GFX_CHECK_HR(hr);
		allocation.reset(alloc);

		if (HasFlag(desc.misc_flags, GfxTextureMiscFlag::Shared))
		{
			hr = gfx->GetDevice()->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &shared_handle);
			GFX_CHECK_HR(hr);
		}

		if (desc.heap_type == GfxResourceUsage::Readback)
		{
			hr = resource->Map(0, nullptr, &mapped_data);
			GFX_CHECK_HR(hr);
		}
		else if (desc.heap_type == GfxResourceUsage::Upload)
		{
			D3D12_RANGE read_range = {};
			hr = resource->Map(0, &read_range, &mapped_data);
			GFX_CHECK_HR(hr);
		}
		if (desc.mip_levels == 0)
		{
			const_cast<GfxTextureDesc&>(desc).mip_levels = (Uint32)log2(std::max<Uint32>(desc.width, desc.height)) + 1;
		}

		GfxLinearDynamicAllocator* dynamic_allocator = gfx->GetDynamicAllocator();
		GfxCommandList* cmd_list = gfx->GetGraphicsCommandList();
		if (data.sub_data != nullptr)
		{
			Uint32 subresource_count = data.sub_count;
			if (subresource_count == Uint32(-1)) subresource_count = desc.array_size * std::max<Uint32>(1u, desc.mip_levels);
			Uint64 required_size;
			device->GetCopyableFootprints(&resource_desc, 0, (Uint32)subresource_count, 0, nullptr, nullptr, nullptr, &required_size);
			GfxDynamicAllocation dyn_alloc = dynamic_allocator->Allocate(required_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

			std::vector<D3D12_SUBRESOURCE_DATA> subresource_data(subresource_count);
			for (Uint32 i = 0; i < subresource_count; ++i)
			{
				GfxTextureSubData init_data = data.sub_data[i];
				subresource_data[i].pData = init_data.data;
				subresource_data[i].RowPitch = init_data.row_pitch;
				subresource_data[i].SlicePitch = init_data.slice_pitch;
			}
			UpdateSubresources(cmd_list->GetNative(), resource.Get(), (ID3D12Resource*)dyn_alloc.buffer->GetNative(), dyn_alloc.offset, 0, subresource_count, subresource_data.data());

			if (desc.initial_state != GfxResourceState::CopyDst)
			{
				cmd_list->TextureBarrier(*this, GfxResourceState::CopyDst, desc.initial_state);
				cmd_list->FlushBarriers();
			}
		}
	}

	D3D12Texture::D3D12Texture(GfxDevice* gfx, GfxTextureDesc const& desc) : GfxTexture(gfx, desc)
	{
	}

	D3D12Texture::D3D12Texture(GfxDevice* gfx, GfxTextureDesc const& desc, void* backbuffer) : GfxTexture(gfx, desc, backbuffer), resource((ID3D12Resource*)backbuffer)
	{
	}

	D3D12Texture::~D3D12Texture()
	{
		if (mapped_data != nullptr)
		{
			ADRIA_ASSERT(resource != nullptr);
			resource->Unmap(0, nullptr);
			mapped_data = nullptr;
		}
		if (!is_backbuffer)
		{
			gfx->AddToReleaseQueue(resource.Detach());
			gfx->AddToReleaseQueue(allocation.release());
		}
	}

	void* D3D12Texture::GetNative() const
	{
		return resource.Get();
	}

	void* D3D12Texture::GetSharedHandle() const
	{
		return shared_handle;
	}

	Uint64 D3D12Texture::GetGpuAddress() const
	{
		return resource->GetGPUVirtualAddress();
	}

	Uint32 D3D12Texture::GetRowPitch(Uint32 mip_level) const
	{
		ADRIA_ASSERT(mip_level < desc.mip_levels);
		ID3D12Device* d3d12_device = gfx->GetDevice();
		D3D12_RESOURCE_DESC d3d12_desc = resource->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
		d3d12_device->GetCopyableFootprints(&d3d12_desc, mip_level, 1, 0, &footprint, nullptr, nullptr, nullptr);
		return footprint.Footprint.RowPitch;
	}

	void* D3D12Texture::Map()
	{
		HRESULT hr;
		if (desc.heap_type == GfxResourceUsage::Readback)
		{
			hr = resource->Map(0, nullptr, &mapped_data);
			GFX_CHECK_HR(hr);
		}
		else if (desc.heap_type == GfxResourceUsage::Upload)
		{
			D3D12_RANGE read_range{};
			hr = resource->Map(0, &read_range, &mapped_data);
			GFX_CHECK_HR(hr);
		}
		return mapped_data;
	}

	void D3D12Texture::Unmap()
	{
		resource->Unmap(0, nullptr);
	}

	void D3D12Texture::SetName(Char const* name)
	{
#if defined(_DEBUG) || defined(_PROFILE)
		resource->SetName(ToWideString(name).c_str());
#endif
	}


}