#include "D3D12Fence.h"
#include "Graphics/GfxDevice.h"
#include "Graphics/GfxCommandQueue.h"
#include "Utilities/StringConversions.h"

namespace adria
{
	D3D12Fence::~D3D12Fence()
	{
		CloseHandle(event);
	}

	Bool D3D12Fence::Create(GfxDevice* gfx, Char const* name)
	{
		ID3D12Device* device = (ID3D12Device*)gfx->GetNativeDevice();
		HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
		if (FAILED(hr))
		{
			return false;
		}

		fence->SetName(ToWideString(name).c_str());
		event = CreateEvent(NULL, FALSE, FALSE, NULL);
		return true;
	}

	void D3D12Fence::Wait(Uint64 value)
	{
		if (!IsCompleted(value))
		{
			fence->SetEventOnCompletion(value, event);
			WaitForSingleObjectEx(event, INFINITE, FALSE);
		}
	}

	void D3D12Fence::Signal(Uint64 value)
	{
		fence->Signal(value);
	}

	Bool D3D12Fence::IsCompleted(Uint64 value)
	{
		return GetCompletedValue() >= value;
	}

	Uint64 D3D12Fence::GetCompletedValue() const
	{
		return fence->GetCompletedValue();
	}

	void* D3D12Fence::GetHandle() const
	{
		return fence.Get(); 
	}

}

