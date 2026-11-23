// Discover/activate the HEVC transform shipped in the user's own package.
// Electra enumerates SYNCMFT, not LOCALMFT; registration must be in its Wine prefix.
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID hevc_class{0x420a51a3,0xd605,0x430c,{0xb4,0xfc,0x45,0x27,0x4f,0xa6,0xc5,0x62}};
void check(HRESULT hr,const char* api){if(FAILED(hr)){std::fprintf(stderr,"media: %s: %#lx\n",api,static_cast<unsigned long>(hr));throw hr;}}
HRESULT activate(GUID subtype) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video,subtype};
    IMFActivate** list{};UINT32 count{};
    auto hr=MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,MFT_ENUM_FLAG_SYNCMFT,&input,nullptr,&list,&count);
    if(FAILED(hr))return hr;
    hr=MF_E_TOPO_CODEC_NOT_FOUND;
    for(UINT32 i=0;i<count;++i){
        GUID cls{};list[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute,&cls);
        if(cls==hevc_class){ComPtr<IMFTransform> decoder;hr=list[i]->ActivateObject(IID_PPV_ARGS(&decoder));}
        list[i]->Release();
    }
    CoTaskMemFree(list);return hr;
}
HRESULT register_class(const wchar_t* dll,const wchar_t* server,const std::vector<MFT_REGISTER_TYPE_INFO>& inputs){
    wchar_t guid[40];StringFromGUID2(hevc_class,guid,40);
    auto path=L"Software\\Classes\\CLSID\\"+std::wstring(guid)+L"\\InprocServer32";
    HKEY h{};DWORD disposition{};
    auto res=RegCreateKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&h,&disposition);
    if(res)return HRESULT_FROM_WIN32(res);
    if(disposition==REG_OPENED_EXISTING_KEY){
        DWORD size=0,type=0;res=RegQueryValueExW(h,nullptr,nullptr,&type,nullptr,&size);
        if(!res && type==REG_SZ && size && size<65536){
            std::vector<wchar_t> value(size/2+1,0);
            res=RegQueryValueExW(h,nullptr,nullptr,&type,reinterpret_cast<BYTE*>(value.data()),&size);
            if(!res && _wcsicmp(value.data(),dll) && _wcsicmp(value.data(),server)){
                std::vector<wchar_t> prev(32768),original(32768);
                DWORD prev_size=DWORD(prev.size()*sizeof(wchar_t)),orig_size=prev_size;
                if(RegGetValueW(h,nullptr,L"FrostySurfaceBridge",RRF_RT_REG_SZ,nullptr,prev.data(),&prev_size) ||
                   RegGetValueW(h,nullptr,L"FrostyOriginalDecoder",RRF_RT_REG_SZ,nullptr,original.data(),&orig_size) ||
                   _wcsicmp(value.data(),prev.data()) || _wcsicmp(dll,original.data()))res=ERROR_ALREADY_EXISTS;
            }
        }else res=ERROR_ALREADY_EXISTS;
    }
    if(!res){
        if(_wcsicmp(dll,server))res=RegSetValueExW(h,L"FrostyOriginalDecoder",0,REG_SZ,reinterpret_cast<const BYTE*>(dll),DWORD(2*(wcslen(dll)+1)));
        if(!res && _wcsicmp(dll,server))res=RegSetValueExW(h,L"FrostySurfaceBridge",0,REG_SZ,reinterpret_cast<const BYTE*>(server),DWORD(2*(wcslen(server)+1)));
        if(!res)res=RegSetValueExW(h,nullptr,0,REG_SZ,reinterpret_cast<const BYTE*>(server),DWORD(2*(wcslen(server)+1)));
        if(!res)res=RegSetValueExW(h,L"ThreadingModel",0,REG_SZ,reinterpret_cast<const BYTE*>(L"Both"),sizeof(L"Both"));
    }
    RegCloseKey(h);
    auto hr=HRESULT_FROM_WIN32(res);
    if(SUCCEEDED(hr)) {
        wchar_t name[]=L"Packaged HEVC decoder";
        hr=MFTRegister(hevc_class,MFT_CATEGORY_VIDEO_DECODER,name,MFT_ENUM_FLAG_SYNCMFT,
            UINT32(inputs.size()),const_cast<MFT_REGISTER_TYPE_INFO*>(inputs.data()),0,nullptr,nullptr);
    }
    if(FAILED(hr) && disposition==REG_CREATED_NEW_KEY)RegDeleteKeyW(HKEY_LOCAL_MACHINE,path.c_str());
    return hr;
}
}
int wmain(int argc,wchar_t** argv) try {
    if(argc<2||argc>4||(argc>=3 && wcscmp(argv[2],L"--register")))return 2;
    check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitializeEx");check(MFStartup(MF_VERSION),"MFStartup");
    auto module=LoadLibraryW(argv[1]);if(!module)check(HRESULT_FROM_WIN32(GetLastError()),"LoadLibrary(package decoder)");
    auto factory=std::bit_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
    if(!factory)check(E_NOINTERFACE,"DllGetClassObject export");
    ComPtr<IClassFactory> cls;ComPtr<IMFTransform> mft;check(factory(hevc_class,IID_PPV_ARGS(&cls)),"DllGetClassObject");
    check(cls->CreateInstance(nullptr,IID_PPV_ARGS(&mft)),"create original transform");
    std::vector<MFT_REGISTER_TYPE_INFO> inputs;
    for(DWORD i=0;;++i){
        ComPtr<IMFMediaType> type;auto hr=mft->GetInputAvailableType(0,i,&type);if(hr==MF_E_NO_MORE_TYPES)break;
        check(hr,"GetInputAvailableType");MFT_REGISTER_TYPE_INFO t{};
        check(type->GetGUID(MF_MT_MAJOR_TYPE,&t.guidMajorType),"input major type");
        check(type->GetGUID(MF_MT_SUBTYPE,&t.guidSubtype),"input subtype");inputs.push_back(t);
    }
    GUID hevc=MFVideoFormat_NV12;hevc.Data1=0x43564548;
    if(!std::any_of(inputs.begin(),inputs.end(),[&](auto& t){return t.guidMajorType==MFMediaType_Video && t.guidSubtype==hevc;}))
        check(MF_E_INVALIDMEDIATYPE,"decoder has no HEVC input");
    std::printf("Packaged decoder: constructed IMFTransform, %zu input subtypes, HEVC present\n",inputs.size());
    if(argc>=3)check(register_class(argv[1],argc==4?argv[3]:argv[1],inputs),"register in prefix");
    check(activate(hevc),"SYNCMFT enumeration and activation");
    std::puts("Packaged decoder: activated from Electra's HEVC filter");
    mft.Reset();cls.Reset();MFShutdown();CoUninitialize();return 0;
}catch(HRESULT){return 1;}
