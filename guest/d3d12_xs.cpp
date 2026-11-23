#include <windows.h>
#include <cstdio>

// Frosty imports the GUID's storage by name. This is not a function or
// a pointer to a separate GUID allocation.
extern "C" const GUID IID_ID3D12Device = {
    0x189819f1, 0x1db6, 0x4b57, {0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7}
};

extern "C" HRESULT WINAPI D3D12XboxSetProcessDebugFlags(UINT flags) {
    if (flags == 0) return S_OK;
    std::fprintf(stderr, "frosty/d3d12_xs: unsupported process debug flags %#x\n", flags);
    return E_NOTIMPL;
}
