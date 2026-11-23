#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <memory>
#include <atomic>
#include <deque>
#include <vector>
#include <unordered_set>
#include <map>
#include <span>
#include "gpu_detile.hpp"

struct TextureAccess {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    // Accessed under XboxTextures::lock_ except for the lock-free fast path.
    UINT64 epoch{};
    bool immutable{};
    std::atomic<bool> settled{};
    std::map<ID3D12Fence*,std::pair<Ptr<ID3D12Fence>,UINT64>> readers;
};
struct TextureReads {
    std::vector<std::shared_ptr<TextureAccess>> images;
    std::unordered_set<TextureAccess*> seen;
    void add(const std::shared_ptr<TextureAccess>& a){
        if(a && !a->settled.load() && seen.insert(a.get()).second)images.push_back(a);
    }
    void clear(){images.clear();seen.clear();}
};
struct UploadConsumer {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 serial{},seen{};
};

struct TextureUse {
    // An unpublished descriptor can be copied, overwritten or discarded.
    // Hold its resource only until its referenced subresources are ready.
    Microsoft::WRL::ComPtr<ID3D12Resource> pending;
    std::mutex lock;
    UINT first_mip{},mip_count{},first_layer{},layers{};
    std::atomic<bool> ready{};
    std::shared_ptr<TextureAccess> access;
};

class XboxTextures {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    std::mutex lock_;
    GpuDetiler detiler_;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12Fence> fence_;
    UINT64 serial_{};
    struct Batch {
        Ptr<ID3D12CommandAllocator> alloc;
        Ptr<ID3D12GraphicsCommandList> list;
        std::vector<Ptr<ID3D12Resource>> keep;
        std::vector<Ptr<ID3D12Fence>> waits;
        std::vector<std::shared_ptr<TextureAccess>> images;
        UINT64 bytes{},value{};
    };
    Batch pending_;
    std::deque<Batch> flight_;
    UINT64 in_flight_{};
    HRESULT begin_locked(ID3D12Device*);
    HRESULT flush_locked();
    HRESULT reap_locked();
public:
    ~XboxTextures();
    HRESULT load(ID3D12Device*,const BYTE* address,const D3D12_RESOURCE_DESC&,
                 D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    std::shared_ptr<TextureUse> shader_view(ID3D12Resource*,const D3D12_SHADER_RESOURCE_VIEW_DESC*);
    HRESULT refresh(ID3D12Device*,ID3D12Resource*,TextureUse* use=nullptr);
    std::shared_ptr<TextureAccess> access(ID3D12Resource*);
    HRESULT submit(ID3D12Device*,ID3D12CommandQueue*,UploadConsumer&,std::span<const TextureReads* const>,
                   UINT,ID3D12CommandList* const*);
};
