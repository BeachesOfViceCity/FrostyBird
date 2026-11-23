#pragma once
#include <d3d12.h>
#include <array>

D3D12_RESOURCE_DESC desktop_desc(const D3D12_RESOURCE_DESC&);
// The packaged XS validation layer identifies bits 25..29 as the texture
// compression preserve flags. Host D3D12/Vulkan owns compression metadata.
constexpr UINT xs_compression_preserve=0x3e000000u;
inline D3D12_RESOURCE_STATES desktop_state(D3D12_RESOURCE_DIMENSION dim,D3D12_RESOURCE_STATES state){
    if(dim<D3D12_RESOURCE_DIMENSION_TEXTURE1D || dim>D3D12_RESOURCE_DIMENSION_TEXTURE3D || UINT(state)==UINT_MAX)return state;
    return static_cast<D3D12_RESOURCE_STATES>(UINT(state)&~xs_compression_preserve);
}
void tag_depth_alias(ID3D12Resource*,const D3D12_RESOURCE_DESC&);
// S_FALSE = ordinary resource/barrier, S_OK = translated.
HRESULT depth_uav_view(ID3D12Device*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
struct BarrierBatch{
    ID3D12GraphicsCommandList* list;
    std::array<D3D12_RESOURCE_BARRIER,32> pending;
    UINT n{};
    explicit BarrierBatch(ID3D12GraphicsCommandList* l):list(l){}
    void flush(){if(n){list->ResourceBarrier(n,pending.data());n=0;}}
    void add(const D3D12_RESOURCE_BARRIER& b){if(n==pending.size())flush();pending[n++]=b;}
};
HRESULT depth_barrier(ID3D12Device*,BarrierBatch&,const D3D12_RESOURCE_BARRIER&);
