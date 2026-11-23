#pragma once
#include <d3d12.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct RayPipelineLibrary {
    std::wstring entry,alias,group;
    std::vector<BYTE> code;
};

// Portable recipes contain original bytecode and pipeline state. Host PSOs
// themselves never cross a device/driver/translator boundary.
class PipelinePrep {
    struct State;
    std::unique_ptr<State> state_;
public:
    PipelinePrep();
    ~PipelinePrep();
    void start(ID3D12Device*,bool retain=true);
    // The offline helper owns a large stack and a process dedicated to baking.
    void bake(ID3D12Device*,unsigned first=0);
    // Synchronous only for the offline regression executable.
    UINT join_and_count_hits();
    HRESULT compute(ID3D12Device*,const D3D12_COMPUTE_PIPELINE_STATE_DESC&,UINT64,ID3D12PipelineState**);
    HRESULT graphics(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,BOOL,ID3D12PipelineState**);
    HRESULT rays(ID3D12Device5*,ID3D12RootSignature*,D3D12_RAYTRACING_SHADER_CONFIG,D3D12_RAYTRACING_PIPELINE_CONFIG1,
                 const std::vector<RayPipelineLibrary>&,UINT raygen,std::span<const BYTE> local_root,ID3D12StateObject**);
};
void remember_pipeline_root(ID3D12RootSignature*,UINT,const void*,SIZE_T);
void pipeline_view_hint(const std::wstring& path,std::span<const BYTE> types);
