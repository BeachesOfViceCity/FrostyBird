#pragma once
#include <d3d12.h>
#include <array>
#include <string>
#include <vector>

HRESULT xs_rt_collection(ID3D12Device5*,IUnknown* xs_device,const void*,UINT,ID3D12RootSignature*,UINT,REFIID,void**);
HRESULT xs_rt_pipeline(ID3D12Device5*,IUnknown* xs_device,const D3D12_STATE_OBJECT_DESC*,REFIID,void**);
HRESULT xs_rt_extend(ID3D12Device7*,IUnknown* xs_device,const D3D12_STATE_OBJECT_DESC*,ID3D12StateObject*,REFIID,void**);
ID3D12StateObject* native_rt_state(ID3D12StateObject*);
// A title may prepare whole libraries which it never dispatches. Resolve the
// recorded recipe at first dispatch; never pass a guest state object to Vulkan.
HRESULT resolve_rt_state(ID3D12StateObject*,ID3D12StateObject**);
struct RayIdentifier {std::array<BYTE,32> id;std::wstring name;UINT64 key;};
std::vector<RayIdentifier> rt_identifiers(ID3D12StateObject*);
bool rt_settings(ID3D12StateObject*,D3D12_RAYTRACING_SHADER_CONFIG&,D3D12_RAYTRACING_PIPELINE_CONFIG1&);
