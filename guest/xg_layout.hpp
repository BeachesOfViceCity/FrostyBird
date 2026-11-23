#pragma once
#include <d3d12.h>
#include <bit>
#include "tile_plan.hpp"

// Scarlett XG texture address computer supplied in EmbeddedXvd.
struct XgComputer {
    struct Vtable {
        ULONG (WINAPI *addref)(XgComputer*);
        ULONG (WINAPI *release)(XgComputer*);
        HRESULT (WINAPI *layout)(XgComputer*,void*);
        UINT64 (WINAPI *size)(XgComputer*);
        UINT64 (WINAPI *alignment)(XgComputer*);
        UINT64 (WINAPI *mip_offset)(XgComputer*,UINT,UINT);
        UINT64 (WINAPI *offset)(XgComputer*,UINT,UINT,UINT64,UINT,UINT,UINT,void*);
        void* coordinates;
        void* copy_into;
        HRESULT (WINAPI *copy_from)(XgComputer*,void*,UINT,UINT,UINT,const void*,UINT,UINT);
    };
    const Vtable* v;
    ULONG AddRef(){return v->addref(this);}
    ULONG Release(){return v->release(this);}
};
inline HRESULT xg_computer(const D3D12_RESOURCE_DESC& xs,XgComputer** result){
    using Fn=HRESULT(WINAPI*)(const D3D12_RESOURCE_DESC*,XgComputer**);
    auto f=std::bit_cast<Fn>(GetProcAddress(GetModuleHandleW(L"xg_xs.dll"),"XGCreateTextureComputer"));
    return f?f(&xs,result):HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
inline TileKey tile_key(const D3D12_RESOURCE_DESC& xs,UINT64 bytes,UINT sub,UINT64 columns,UINT rows,UINT depth,UINT elem,UINT64 first){
    return {xs.Dimension,xs.Alignment,xs.Width,xs.Height,xs.DepthOrArraySize,xs.MipLevels,xs.Format,xs.SampleDesc.Count,
            xs.SampleDesc.Quality,xs.Layout,xs.Flags,bytes,sub,columns,rows,depth,elem,first};
}
