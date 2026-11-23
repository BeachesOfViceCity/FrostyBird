// The shipping MFT decodes HEVC in software without an Xbox video engine.
// Expose its actual NV12 output as the two D3D12 planes consumed by Electra.
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <mfd3d12.h>
#include <wrl/client.h>
#include "video_surface.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
using Microsoft::WRL::ComPtr;
namespace {
constexpr GUID codec{0x420a51a3,0xd605,0x430c,{0xb4,0xfc,0x45,0x27,0x4f,0xa6,0xc5,0x62}};
constexpr GUID sync_key{0x2a7c8d6a,0x85a6,0x494d,{0xa0,0x46,0x06,0xea,0x1a,0x13,0x8f,0x4b}};
void notice(const char* text,HRESULT hr=S_OK){
    char msg[240];std::snprintf(msg,sizeof(msg),"frosty/video: %s [%#lx]\n",text,(unsigned long)hr);
    OutputDebugStringA(msg);
    DWORD written;WriteFile(GetStdHandle(STD_ERROR_HANDLE),msg,DWORD(std::strlen(msg)),&written,nullptr);
}
struct FrameBuffer final:IMFMediaBuffer,IMFDXGIBuffer {
    std::atomic<ULONG> refs{1};
    ComPtr<IMFMediaBuffer> cpu;
    ComPtr<ID3D12Resource> surface;
    ComPtr<IMFAttributes> attributes;
    HRESULT WINAPI QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;
        *out=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(IMFMediaBuffer))*out=static_cast<IMFMediaBuffer*>(this);
        else if(id==__uuidof(IMFDXGIBuffer))*out=static_cast<IMFDXGIBuffer*>(this);else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG WINAPI AddRef()override{return ++refs;}
    ULONG WINAPI Release()override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT WINAPI Lock(BYTE** b,DWORD* max,DWORD* n)override{return cpu->Lock(b,max,n);}
    HRESULT WINAPI Unlock()override{return cpu->Unlock();}
    HRESULT WINAPI GetCurrentLength(DWORD* n)override{return cpu->GetCurrentLength(n);}
    HRESULT WINAPI SetCurrentLength(DWORD n)override{return cpu->SetCurrentLength(n);}
    HRESULT WINAPI GetMaxLength(DWORD* n)override{return cpu->GetMaxLength(n);}
    HRESULT WINAPI GetResource(REFIID id,void** out)override{return surface->QueryInterface(id,out);}
    HRESULT WINAPI GetSubresourceIndex(UINT* n)override{if(!n)return E_POINTER;*n=0;return S_OK;}
    HRESULT WINAPI GetUnknown(REFGUID key,REFIID iid,void** out)override{return attributes->GetUnknown(key,iid,out);}
    HRESULT WINAPI SetUnknown(REFGUID key,IUnknown* value)override{return value?attributes->SetUnknown(key,value):attributes->DeleteItem(key);}
};
struct Uploader {
    using MakeSurface=decltype(&FrostyMakeVideoSurface);
    MakeSurface make_surface{};
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 serial{};
    HRESULT set_manager(IUnknown* obj){
        if(!obj){*this={};return S_OK;}
        ComPtr<IMFDXGIDeviceManager> m;auto hr=obj->QueryInterface(IID_PPV_ARGS(&m));
        if(FAILED(hr))return hr;
        HANDLE handle{};if(FAILED(hr=m->OpenDeviceHandle(&handle)))return hr;
        ComPtr<ID3D12Device> guest;hr=m->GetVideoService(handle,IID_PPV_ARGS(&guest));m->CloseDeviceHandle(handle);
        if(FAILED(hr))return hr;
        Uploader next;
        // COM may activate the transform in a process which only enumerates
        // codecs. Resolve D3D12X from the title only when a manager is bound.
        auto xs=GetModuleHandleW(L"d3d12_xs.dll");
        if(!xs)return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        auto get=std::bit_cast<decltype(&FrostyGetVideoDevice)>(GetProcAddress(xs,"FrostyGetVideoDevice"));
        next.make_surface=std::bit_cast<MakeSurface>(GetProcAddress(xs,"FrostyMakeVideoSurface"));
        if(!get||!next.make_surface)return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        if(FAILED(hr=get(guest.Get(),&next.dev)))return hr;
        D3D12_COMMAND_QUEUE_DESC q{};
        if(FAILED(hr=next.dev->CreateCommandQueue(&q,IID_PPV_ARGS(&next.queue))) ||
           FAILED(hr=next.dev->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&next.alloc))) ||
           FAILED(hr=next.dev->CreateCommandList(0,q.Type,next.alloc.Get(),nullptr,IID_PPV_ARGS(&next.list))) ||
           FAILED(hr=next.list->Close()) || FAILED(hr=next.dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&next.fence))))return hr;
        *this=std::move(next);return S_OK;
    }
    HRESULT frame(IMFSample* sample,IMFMediaType* mt){
        UINT w=0,h=0,pitch=0;GUID fmt{};
        auto hr=mt->GetGUID(MF_MT_SUBTYPE,&fmt);if(FAILED(hr)||fmt!=MFVideoFormat_NV12)return MF_E_INVALIDMEDIATYPE;
        if(FAILED(hr=MFGetAttributeSize(mt,MF_MT_FRAME_SIZE,&w,&h)))return hr;
        if(FAILED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE,&pitch)))pitch=w;
        if(!w||!h||w>8192||h>8192||w%2||h%2||pitch<w||pitch>16384)return MF_E_INVALIDMEDIATYPE;
        unsigned pw=(w+255u)&~255u,ph=(h+15u)&~15u;UINT64 plane_bytes=UINT64(pw)*ph;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_BUFFER,0,plane_bytes+plane_bytes/2,1,1,1,DXGI_FORMAT_UNKNOWN,
                               {1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        ComPtr<ID3D12Resource> storage;
        if(FAILED(hr=dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&storage))))return hr;
        ComPtr<IMFMediaBuffer> cpu;if(FAILED(hr=sample->ConvertToContiguousBuffer(&cpu)))return hr;
        BYTE* src{};DWORD length{};if(FAILED(hr=cpu->Lock(&src,nullptr,&length)))return hr;
        if(UINT64(pitch)*h*3/2>length){cpu->Unlock();return MF_E_BUFFERTOOSMALL;}
        D3D12_RANGE none{};BYTE* pixels{};
        if(FAILED(hr=storage->Map(0,&none,reinterpret_cast<void**>(&pixels)))){cpu->Unlock();return hr;}
        for(unsigned p=0;p<2;++p){
            auto* d=pixels+(p?plane_bytes:0);auto* s=src+(p?UINT64(pitch)*h:0);
            unsigned n=p?h/2:h,padded=p?ph/2:ph;
            if(w==pitch && pw==w)std::memcpy(d,s,size_t(w)*n);
            else for(unsigned row=0;row<n;++row){std::memcpy(d+size_t(row)*pw,s+size_t(row)*pitch,w);std::memset(d+size_t(row)*pw+w,p?128:16,pw-w);}
            if(padded>n)std::memset(d+size_t(n)*pw,p?128:16,size_t(padded-n)*pw);
        }
        storage->Unmap(0,nullptr);cpu->Unlock();
        hp.Type=D3D12_HEAP_TYPE_DEFAULT;std::array<ComPtr<ID3D12Resource>,2> plane;
        if(FAILED(hr=alloc->Reset())||FAILED(hr=list->Reset(alloc.Get(),nullptr)))return hr;
        for(unsigned p=0;p<2;++p){
            auto format=p?DXGI_FORMAT_R8G8_UNORM:DXGI_FORMAT_R8_UNORM;
            auto tw=p?pw/2:pw,th=p?ph/2:ph;
            D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D,0,tw,th,1,1,format,{1,0},D3D12_TEXTURE_LAYOUT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE};
            if(FAILED(hr=dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&plane[p]))))return hr;
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=storage.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint={p?plane_bytes:0,{format,tw,th,1,pw}};to.pResource=plane[p].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            D3D12_RESOURCE_BARRIER bar{};bar.Transition={plane[p].Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,p?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_COMMON};
            list->ResourceBarrier(1,&bar);
        }
        ComPtr<FrameBuffer> fb;fb.Attach(new(std::nothrow) FrameBuffer);if(!fb)return E_OUTOFMEMORY;
        fb->cpu=cpu;
        if(FAILED(hr=make_surface(plane[0].Get(),plane[1].Get(),storage.Get(),plane_bytes,&fb->surface)))return hr;
        using MakeSync=HRESULT(WINAPI*)(ID3D12Device*,REFIID,void**);
        static auto sync=std::bit_cast<MakeSync>(GetProcAddress(GetModuleHandleW(L"mfplat.dll"),"MFCreateD3D12SynchronizationObject"));
        if(!sync)return E_NOTIMPL;
        ComPtr<IMFD3D12SynchronizationObjectCommands> commands;
        if(FAILED(hr=sync(dev.Get(),IID_PPV_ARGS(&commands))) || FAILED(hr=MFCreateAttributes(&fb->attributes,1)) ||
           FAILED(hr=fb->attributes->SetUnknown(sync_key,commands.Get())) || FAILED(hr=list->Close()))return hr;
        ID3D12CommandList* submission=list.Get();queue->ExecuteCommandLists(1,&submission);
        if(FAILED(hr=commands->EnqueueResourceReady(queue.Get())) || FAILED(hr=queue->Signal(fence.Get(),++serial)) ||
           FAILED(hr=fence->SetEventOnCompletion(serial,nullptr)))return hr;
        if(FAILED(hr=sample->RemoveAllBuffers()) || FAILED(hr=sample->AddBuffer(static_cast<IMFMediaBuffer*>(fb.Get()))))return hr;
        if(serial==1 || serial==32 || serial==256){
            char msg[120];std::snprintf(msg,sizeof(msg),"uploaded decoded NV12 frame %llu: %ux%u, %lu bytes",serial,w,h,length);notice(msg);
        }
        return S_OK;
    }
};
struct Decoder final:IMFTransform {
    std::atomic<ULONG> refs{1};ComPtr<IMFTransform> core;Uploader gpu;
    HRESULT WINAPI QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;
        *out=nullptr;if(id!=__uuidof(IUnknown)&&id!=__uuidof(IMFTransform))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef()override{return ++refs;}ULONG WINAPI Release()override{auto n=--refs;if(!n)delete this;return n;}
#define FORWARD(name,args,params) HRESULT WINAPI name args override {return core->name params;}
    FORWARD(GetStreamLimits,(DWORD* a,DWORD* b,DWORD* c,DWORD* d),(a,b,c,d))
    FORWARD(GetStreamCount,(DWORD* a,DWORD* b),(a,b))
    FORWARD(GetStreamIDs,(DWORD a,DWORD* b,DWORD c,DWORD* d),(a,b,c,d))
    FORWARD(GetInputStreamInfo,(DWORD i,MFT_INPUT_STREAM_INFO* x),(i,x))
    FORWARD(GetAttributes,(IMFAttributes** a),(a))
    FORWARD(GetInputStreamAttributes,(DWORD i,IMFAttributes** a),(i,a))
    FORWARD(GetOutputStreamAttributes,(DWORD i,IMFAttributes** a),(i,a))
    FORWARD(DeleteInputStream,(DWORD i),(i))
    FORWARD(AddInputStreams,(DWORD n,DWORD* ids),(n,ids))
    FORWARD(GetInputAvailableType,(DWORD i,DWORD n,IMFMediaType** t),(i,n,t))
    FORWARD(GetOutputAvailableType,(DWORD i,DWORD n,IMFMediaType** t),(i,n,t))
    FORWARD(SetInputType,(DWORD i,IMFMediaType* t,DWORD flags),(i,t,flags))
    FORWARD(SetOutputType,(DWORD i,IMFMediaType* t,DWORD flags),(i,t,flags))
    FORWARD(GetInputCurrentType,(DWORD i,IMFMediaType** t),(i,t))
    FORWARD(GetOutputCurrentType,(DWORD i,IMFMediaType** t),(i,t))
    FORWARD(GetInputStatus,(DWORD i,DWORD* flags),(i,flags))
    FORWARD(GetOutputStatus,(DWORD* flags),(flags))
    FORWARD(SetOutputBounds,(LONGLONG lo,LONGLONG hi),(lo,hi))
    FORWARD(ProcessEvent,(DWORD i,IMFMediaEvent* e),(i,e))
    FORWARD(ProcessInput,(DWORD i,IMFSample* s,DWORD flags),(i,s,flags))
#undef FORWARD
    HRESULT WINAPI GetOutputStreamInfo(DWORD stream,MFT_OUTPUT_STREAM_INFO* info)override{
        auto hr=core->GetOutputStreamInfo(stream,info);
        if(SUCCEEDED(hr)&&gpu.dev)info->dwFlags=(info->dwFlags&~MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)|MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
        return hr;
    }
    HRESULT WINAPI ProcessMessage(MFT_MESSAGE_TYPE msg,ULONG_PTR param)override{
        if(msg!=MFT_MESSAGE_SET_D3D_MANAGER)return core->ProcessMessage(msg,param);
        auto hr=gpu.set_manager(reinterpret_cast<IUnknown*>(param));
        notice(param?"configured D3D12 output for packaged decoder":"released decoder output device",hr);return hr;
    }
    HRESULT WINAPI ProcessOutput(DWORD flags,DWORD count,MFT_OUTPUT_DATA_BUFFER* output,DWORD* status)override{
        if(!gpu.dev)return core->ProcessOutput(flags,count,output,status);
        if(!output||!status||count!=1||output->pSample)return E_INVALIDARG;
        MFT_OUTPUT_STREAM_INFO info{};auto hr=core->GetOutputStreamInfo(output->dwStreamID,&info);
        if(FAILED(hr))return hr;
        ComPtr<IMFSample> sample;MFT_OUTPUT_DATA_BUFFER data{};data.dwStreamID=output->dwStreamID;
        if(!(info.dwFlags&MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)){
            ComPtr<IMFMediaBuffer> b;
            if(!info.cbSize || info.cbSize>(256u<<20))return MF_E_INVALIDMEDIATYPE;
            if(FAILED(hr=MFCreateSample(&sample)) || FAILED(hr=MFCreateAlignedMemoryBuffer(info.cbSize,
                std::max<DWORD>(63,info.cbAlignment?info.cbAlignment-1:0),&b)) || FAILED(hr=sample->AddBuffer(b.Get())))return hr;
            data.pSample=sample.Get();
        }
        hr=core->ProcessOutput(flags,1,&data,status);
        output->dwStatus=data.dwStatus;output->pEvents=data.pEvents;
        if(data.pSample!=sample.Get())sample.Attach(data.pSample);
        if(FAILED(hr) || (output->dwStatus&MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE)==MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE)return hr;
        if(!sample)return E_UNEXPECTED;
        ComPtr<IMFMediaType> mt;
        if(FAILED(hr=core->GetOutputCurrentType(output->dwStreamID,&mt)) || FAILED(hr=gpu.frame(sample.Get(),mt.Get()))){
            notice("decoded frame surface transfer failed",hr);return hr;
        }
        output->pSample=sample.Detach();return S_OK;
    }
};
class Factory final:public IClassFactory {
public:
    HRESULT WINAPI QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;
        *out=nullptr;if(id!=__uuidof(IUnknown)&&id!=__uuidof(IClassFactory))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef()override{return 2;}ULONG WINAPI Release()override{return 1;}
    HRESULT WINAPI LockServer(BOOL)override{return S_OK;}
    HRESULT WINAPI CreateInstance(IUnknown* outer,REFIID iid,void** out)override{
        if(!out)return E_POINTER;
        *out=nullptr;if(outer)return CLASS_E_NOAGGREGATION;
        static auto dll=[]{
            std::array<wchar_t,32768> path{};DWORD size=sizeof(path);
            if(RegGetValueW(HKEY_LOCAL_MACHINE,L"Software\\Classes\\CLSID\\{420A51A3-D605-430C-B4FC-45274FA6C562}\\InprocServer32",
                    L"FrostyOriginalDecoder",RRF_RT_REG_SZ,nullptr,path.data(),&size)!=ERROR_SUCCESS)return HMODULE{};
            return LoadLibraryW(path.data());
        }();
        using Get=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
        static auto get=dll?std::bit_cast<Get>(GetProcAddress(dll,"DllGetClassObject")):nullptr;
        if(!get)return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        ComPtr<IClassFactory> f;auto hr=get(codec,IID_PPV_ARGS(&f));if(FAILED(hr))return hr;
        ComPtr<Decoder> d;d.Attach(new(std::nothrow) Decoder);if(!d)return E_OUTOFMEMORY;
        if(FAILED(hr=f->CreateInstance(nullptr,IID_PPV_ARGS(&d->core))))return hr;
        return d->QueryInterface(iid,out);
    }
};
}
extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID cls,REFIID id,void** out){
    if(cls!=codec)return CLASS_E_CLASSNOTAVAILABLE;
    static Factory f;return f.QueryInterface(id,out);
}
extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(){return S_FALSE;}
