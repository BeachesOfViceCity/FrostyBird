#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <span>

struct RayRowBuffers {
    Microsoft::WRL::ComPtr<ID3D12Resource> packed,readback;
    UINT64 capacity{};
};
class RayRowPacker {
    struct State;std::unique_ptr<State> impl_;
public:
    RayRowPacker();
    ~RayRowPacker();
    bool init(ID3D12Device*);
    std::shared_ptr<RayRowBuffers> record(ID3D12Device*,ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64 offset,UINT count);
    static UINT groups(UINT count){return count/128+(count%128!=0);}
    static UINT64 bytes(UINT count){return 4ull*129*groups(count);}
    template<class F>static bool visit(std::span<const UINT> words,UINT count,F consume){
        if(words.size()!=bytes(count)/4)return false;
        for(UINT i=0;i<groups(count);++i){
            auto block=words.subspan(size_t(i)*129,129);UINT n=block[0];
            if(n>128 || n>count-i*128)return false;
            for(UINT j=1;j<=n;++j){UINT t=block[j];if(t>>25)return false;consume(t&0xffffffu,!(t&0x1000000u));}
        }
        return true;
    }
};
