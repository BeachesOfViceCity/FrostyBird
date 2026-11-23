#pragma once
#include "xg_layout.hpp"
#include <wrl/client.h>
#include <deque>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <set>

struct GpuTileView{
    Microsoft::WRL::ComPtr<ID3D12Resource> map;
    UINT low{},high{};size_t bytes{};
};
struct GpuTileWork{
    std::shared_ptr<const GpuTileView> view;
    Microsoft::WRL::ComPtr<ID3D12Resource> tiled,linear;
    std::array<UINT,5> constants{};
    UINT64 bytes{};
};
// Optional compiler-only preparation. No texture data is needed to measure
// the layout. Groups of 16 elements are encoded by a base and an index into
// a dictionary of exact XG offsets (no periodicity assumption).
bool prepare_gpu_layout(const TileKey&,XgComputer*);
int prepare_known_tile_layouts(ID3D12Device*);
class GpuDetiler{
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    std::mutex lock_;
    std::map<TileKey,std::shared_ptr<GpuTileView>> maps_;
    std::deque<TileKey> fifo_;
    size_t bytes_{};
    std::condition_variable wake_;
    std::deque<TileKey> work_;
    std::set<TileKey> queued_;
    std::thread worker_;
    bool closing_{};
    Ptr<ID3D12RootSignature> root_;
    Ptr<ID3D12PipelineState> kernel_;
    bool init(ID3D12Device*);
    void learn(const TileKey&);
public:
    ~GpuDetiler();
    std::shared_ptr<const GpuTileView> view(ID3D12Device*,const TileKey&,XgComputer*,bool build);
    bool stage(ID3D12Device*,std::shared_ptr<const GpuTileView>,const BYTE* source,UINT cols,UINT rows,UINT pitch,UINT elem,GpuTileWork&);
    void record(ID3D12GraphicsCommandList*,const GpuTileWork&);
};
