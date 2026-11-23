#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <mutex>
#include <utility>

// Layout used by the single-plane PresentX call in the shipping viewport.
struct XsPlane { UINT64 token; UINT count; ID3D12Resource* const* resources; };
class XboxTextures;
struct UploadConsumer;

class Presentation {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<IDXGISwapChain3> swap_;
    struct Slot {
        Ptr<ID3D12CommandAllocator> alloc;
        Ptr<ID3D12GraphicsCommandList> list;
        Ptr<ID3D12Resource> source;
        UINT64 retire{};
    };
    std::array<Slot,3> slots_{};
    Ptr<ID3D12Fence> fence_;
    HWND window_{};
    UINT width_{},height_{};
    DXGI_FORMAT format_{};
    std::mutex lock_;
    bool first_{};
    UINT64 serial_{};
    std::FILE* times_{};
    double hz_{};
    UINT64 last_{},flushed_{};
    UINT displayed_{};
    HRESULT wait_for(UINT64);
    HRESULT init(ID3D12Device*, ID3D12CommandQueue*, IDXGIAdapter*, const D3D12_RESOURCE_DESC&);
public:
    ~Presentation();
    HRESULT present(ID3D12Device*,ID3D12CommandQueue*,IDXGIAdapter*,UINT,const XsPlane*,XboxTextures&,UploadConsumer&);
};
