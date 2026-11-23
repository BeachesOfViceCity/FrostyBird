#pragma once
#include "ray_memory.hpp"
#include "rt_state.hpp"
#include "pipeline_prep.hpp"
#include "ray_packet.hpp"
#include <string>
struct RayLiveView{UINT dimension;std::string type;UINT64 va;};
struct RayExecution {
    Microsoft::WRL::ComPtr<ID3D12StateObject> pipeline;
    std::shared_ptr<RayPacket> packet;
    UINT64 zero_offset{};
    D3D12_DISPATCH_RAYS_DESC desc{};
    UINT64 source{};
    void record(ID3D12GraphicsCommandList4*);
    bool retired();
};
struct RayDispatch {
    RayPacketPool packets;
    struct Library{std::vector<BYTE> source;int triangle_chs_only{-1};};
    struct Identifier {UINT64 key;std::wstring name;};
    std::map<UINT64,Library> libraries;
    std::map<std::array<BYTE,32>,Identifier> ids;
    struct Raygen {
        UINT words{},selector{},field{},value{},as_reg{};
        std::map<UINT,std::pair<UINT,std::string>> layout;
        std::vector<BYTE> code;
    };
    std::map<UINT64,std::vector<Raygen>> raygens;
    struct Linked {Microsoft::WRL::ComPtr<ID3D12RootSignature> root;Microsoft::WRL::ComPtr<ID3D12StateObject> pipeline;bool reported_complete{};};
    std::map<std::string,Linked> cache;
    std::mutex lock;
    void collection(const void*,UINT,ID3D12StateObject*);
    HRESULT link(PipelinePrep&,ID3D12Device5*,ID3D12RootSignature*,ID3D12StateObject*,const D3D12_DISPATCH_RAYS_DESC&,
                 RayMemory&,const std::map<UINT,RayLiveView>&,const std::function<std::shared_ptr<RaySceneLayout>(UINT64)>&,RayExecution&);
};
