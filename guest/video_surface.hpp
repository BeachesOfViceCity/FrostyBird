#pragma once
#include <d3d12.h>

// The XS decoder exposes a luma resource and addresses its chroma plane by
// base GPU VA + aligned luma bytes. The two host textures contain real pixels.
ID3D12Resource* video_native_resource(ID3D12Resource* resource);
HRESULT video_chroma_alias(ID3D12Device* device,UINT64 va,const D3D12_RESOURCE_DESC& desc,REFIID iid,void** out);
extern "C" HRESULT WINAPI FrostyGetVideoDevice(ID3D12Device* guest,ID3D12Device** out);
extern "C" HRESULT WINAPI FrostyMakeVideoSurface(ID3D12Resource* y,ID3D12Resource* uv,ID3D12Resource* storage,
                                                  UINT64 chroma_offset,ID3D12Resource** out);
