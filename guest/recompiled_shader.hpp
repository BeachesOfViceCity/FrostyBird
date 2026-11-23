#pragma once
#include <d3d12.h>
#include <span>
#include <vector>
#include <string>
#include <array>
#include <map>
#include <memory>

// Specialize the scalar return/store type of a view at draw/dispatch time.
enum class Numeric : UINT { floating, unsigned_int, signed_int };
inline constexpr UINT xs_rectangle_pipeline_type=0x13;
UINT64 shader_key(const D3D12_SHADER_BYTECODE&);
UINT shader_compute_wire(ID3D12Device*);
struct ViewBinding { UINT table{~0u}, offset{}; };
struct SrvRange {UINT offset,count;};
enum class ViewStage {compute,vertex,pixel,ray}; // global ray bindings are in space 1
struct RootViews{
    std::array<std::array<ViewBinding,512>,4> slots{};
    std::array<std::vector<SrvRange>,64> ranges;
    std::map<std::pair<ViewStage,UINT>,D3D12_SAMPLER_DESC> samplers;
    UINT words{~0u};
    static constexpr UINT index(UINT r){return (r&127u)|((r&0x300u)>>1);}
};
std::shared_ptr<const RootViews> root_views(ID3D12RootSignature*);
struct TypedView {ViewStage stage;UINT slot;}; // register; bit 8 is set for UAVs
using TypedViews=std::vector<TypedView>;
HRESULT make_compute_variants(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC&, UINT64 key,
                            ID3D12PipelineState**);
bool find_view_binding(const RootViews*,ViewStage,UINT reg,ViewBinding&);
HRESULT create_shader_root(ID3D12Device*,UINT,const void*,SIZE_T,REFIID,void**);
bool table_constant_parameter(const RootViews*,UINT& index);
bool static_sampler(const RootViews*,ViewStage,UINT reg,D3D12_SAMPLER_DESC&);
bool has_view_variants(ID3D12PipelineState*);
void set_pipeline_hint_path(ID3D12PipelineState*,const std::wstring&);
bool get_typed_views(ID3D12PipelineState*,TypedViews&);
HRESULT select_view_variant(ID3D12PipelineState*,std::span<const BYTE> types,ID3D12PipelineState**,bool learn=true);
Numeric numeric_format(DXGI_FORMAT);
HRESULT load_graphics_stage(D3D12_SHADER_BYTECODE&,bool vertex,ID3DBlob** lifetime);
HRESULT make_graphics_variants(ID3D12Device*,const D3D12_GRAPHICS_PIPELINE_STATE_DESC&,ID3D12PipelineState**,BOOL depth_bounds=FALSE);
