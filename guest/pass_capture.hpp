#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct PassReadback {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer,source,counter;
    UINT64 frame{},shader{};
    UINT arg_words{},max_count{},arg_kind{};
};
class PassCapture {
    std::set<UINT64> indirect_;
    std::map<std::pair<UINT64,UINT64>,UINT> arg_samples_;
    std::wstring dir_;
    std::atomic<UINT64> frame_{};
    std::mutex lock_;
public:
    PassCapture();
    bool enabled() const{return !indirect_.empty();}
    void arguments(ID3D12Device*,ID3D12GraphicsCommandList*,UINT64,D3D12_INDIRECT_ARGUMENT_TYPE,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,std::vector<PassReadback>&);
    void save(std::vector<PassReadback>&);
    void present(){if(enabled())++frame_;}
};
