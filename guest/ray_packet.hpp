#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

// The bytes in an SBT belong to one command-list recording. The pool owns
// one reference; each recording and its queue retirement job own another.
struct RayPacket {
    Microsoft::WRL::ComPtr<ID3D12Resource> table,status,readback;
    UINT64 capacity{};
    HRESULT write(std::span<const BYTE> bytes){
        if(bytes.size()<40 || bytes.size()>capacity-16)return E_INVALIDARG;
        void* p{};D3D12_RANGE none{};auto hr=table->Map(0,&none,&p);if(FAILED(hr))return hr;
        auto b=static_cast<BYTE*>(p);auto va=status->GetGPUVirtualAddress();
        std::memcpy(b,bytes.data(),bytes.size());std::memcpy(b+32,&va,sizeof(va));
        std::memset(b+bytes.size(),0,16);
        D3D12_RANGE written{0,bytes.size()+16};table->Unmap(0,&written);return S_OK;
    }
    void begin(ID3D12GraphicsCommandList* l,UINT64 zero_offset)const{
        l->CopyBufferRegion(status.Get(),0,table.Get(),zero_offset,16);
        D3D12_RESOURCE_BARRIER b{};b.Transition={status.Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        l->ResourceBarrier(1,&b);
    }
    void finish(ID3D12GraphicsCommandList* l)const{
        D3D12_RESOURCE_BARRIER b{};b.Transition={status.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        l->ResourceBarrier(1,&b);l->CopyBufferRegion(readback.Get(),0,status.Get(),0,16);
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        l->ResourceBarrier(1,&b);
    }
    HRESULT result(std::array<UINT,4>& v)const{
        D3D12_RANGE range{0,sizeof(v)},none{};void* p{};auto hr=readback->Map(0,&range,&p);
        if(SUCCEEDED(hr)){std::memcpy(v.data(),p,sizeof(v));readback->Unmap(0,&none);}return hr;
    }
};

class RayPacketPool {
    std::vector<std::shared_ptr<RayPacket>> slots_;
public:
    // Serialized by the device ray linker's lock. Large or unusually deep
    // batches take the ordinary allocation path without growing the pool.
    HRESULT take(ID3D12Device* d,UINT64 need,std::shared_ptr<RayPacket>& out,bool& reused){
        if(need<56)return E_INVALIDARG;
        constexpr UINT64 max_table=1u<<20;constexpr UINT max_slots=32;
        auto best=slots_.end(),idle=slots_.end();reused=false;
        if(need<=max_table)for(auto it=slots_.begin();it!=slots_.end();++it)if(it->use_count()==1){
            idle=it;if((*it)->capacity>=need && (best==slots_.end() || (*it)->capacity<(*best)->capacity))best=it;
        }
        if(best!=slots_.end()){out=*best;reused=true;return S_OK;}
        auto slot=std::make_shared<RayPacket>();slot->capacity=need>max_table?need:std::bit_ceil(need);
        D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_DEFAULT;h.CreationNodeMask=h.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC b{D3D12_RESOURCE_DIMENSION_BUFFER,0,16,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        auto hr=d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot->status));
        if(FAILED(hr))return hr;
        h.Type=D3D12_HEAP_TYPE_READBACK;b.Flags=D3D12_RESOURCE_FLAG_NONE;
        hr=d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot->readback));
        if(FAILED(hr))return hr;
        h.Type=D3D12_HEAP_TYPE_UPLOAD;b.Width=slot->capacity;
        hr=d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&slot->table));
        if(FAILED(hr))return hr;
        if(need<=max_table){
            if(slots_.size()<max_slots)slots_.push_back(slot);
            else if(idle!=slots_.end())*idle=slot;
        }
        out=std::move(slot);return S_OK;
    }
};
