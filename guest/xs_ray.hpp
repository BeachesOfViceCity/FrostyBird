#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <bit>

// QI values used by the shipping 210604 XS command-list constructor.
// These revisions have Xbox extension slots, not the desktop 5/6 IIDs.
inline constexpr GUID xs_list5{0x55050859,0x4024,0x474c,{0x98,0xcf,0x64,0x5b,0x51,0xdc,0x48,0x74}};
inline constexpr GUID xs_list6{0xc3827890,0x4024,0x474c,{0x98,0xcf,0x64,0x5b,0x51,0xdc,0x48,0x74}};
inline constexpr GUID xs_serialized_device{0x728f1fa2,0xea11,0x4a9f,{0x9b,0x52,0x9b,0x42,0xa4,0x84,0xd5,0xc9}};
inline constexpr GUID vkd3d_device_ext4{0xa91b5617,0xa06a,0x4d6f,{0xb4,0x37,0x03,0xeb,0xbe,0xf9,0x14,0x15}};
inline constexpr UINT xs_fetch_space=0x7a32, xs_fetch_build_flag=0x80000000u;
inline constexpr UINT xs_group_ordinal=0x46534749u;
inline bool xs_enable_intrinsic(ID3D12Device* dev,UINT opcode){
    Microsoft::WRL::ComPtr<IUnknown> ext;
    if(FAILED(dev->QueryInterface(vkd3d_device_ext4,reinterpret_cast<void**>(ext.GetAddressOf()))))return false;
    auto v=*reinterpret_cast<void***>(ext.Get());
    auto supported=std::bit_cast<BOOL(WINAPI*)(IUnknown*,UINT)>(v[18]);
    auto enable=std::bit_cast<HRESULT(WINAPI*)(IUnknown*,UINT,UINT,BOOL)>(v[19]);
    return supported(ext.Get(),opcode) && SUCCEEDED(enable(ext.Get(),0,xs_fetch_space,FALSE));
}
inline bool xs_enable_position_fetch(ID3D12Device* dev){return xs_enable_intrinsic(dev,98);}
struct XsBatchBuild {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build;
    UINT64 extension[4];
};
static_assert(sizeof(XsBatchBuild)==0x50 && offsetof(XsBatchBuild,extension)==0x30);
