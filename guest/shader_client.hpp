#pragma once
#include <d3d12.h>
#include <span>
#include <vector>
#include <string_view>

// S_FALSE: the input is a native desktop shader, or no compiler was started.
HRESULT request_shader(D3D12_SHADER_BYTECODE,UINT stage,std::span<const BYTE> types,
                       std::vector<UINT>& bindings,ID3DBlob** result);
HRESULT request_ray_leaf(std::span<const BYTE> xsrt,UINT stage,UINT words,UINT word,UINT value,std::string_view symbol,ID3DBlob**);
HRESULT request_raygen(std::span<const BYTE> xsrt,UINT words,UINT route,UINT word,UINT value,std::string_view layout,std::vector<UINT>& bindings,ID3DBlob**);
