#define NOMINMAX
#include "pass_times.hpp"
#include <wrl/client.h>
#include <string>
#include <vector>
#include <charconv>
#include <set>
#include <string_view>

using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT capacity=256;
struct Entry{UINT64 frame;const char* label;ShaderKeys keys;UINT x,y,z;};
struct Chunk{
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> buf;
    std::vector<Entry> entries;
    bool init(ID3D12Device* d){
        D3D12_QUERY_HEAP_DESC q{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2*capacity,0};
        D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_READBACK;h.CreationNodeMask=h.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC b{D3D12_RESOURCE_DIMENSION_BUFFER,0,16*capacity,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        return SUCCEEDED(d->CreateQueryHeap(&q,IID_PPV_ARGS(&heap))) &&
            SUCCEEDED(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buf)));
    }
};
struct Sink{
    std::atomic<UINT64> frame{},first{UINT64_MAX};
    std::mutex lock;FILE* f{};std::wstring path;
    std::set<UINT64> focus;
    Sink(){wchar_t p[1024]{};auto n=GetEnvironmentVariableW(L"FROSTY_PASS_TIMES",p,1024);if(n && n<1024)path.assign(p,n);
        char keys[1024]{};n=GetEnvironmentVariableA("FROSTY_PASS_FOCUS",keys,1024);
        if(n && n<1024){std::string_view left(keys,n);while(!left.empty()){
            auto comma=left.find(',');auto item=left.substr(0,comma);UINT64 key{};
            auto [end,err]=std::from_chars(item.data(),item.data()+item.size(),key,16);
            if(err!=std::errc{} || end!=item.end()){path.clear();break;}focus.insert(key);
            if(comma==left.npos)break;
            left.remove_prefix(comma+1);
        }}
    }
    ~Sink(){if(f)std::fclose(f);}
};
Sink& sink(){static Sink s;return s;}
}
struct TimedRecording{std::vector<Chunk> chunks;};
void PassTimes::presented(){
    auto& s=sink();auto f=++s.frame;
    if(s.path.empty() || s.first!=UINT64_MAX || (f%8))return;
    if(GetFileAttributesW((s.path+L".on").c_str())==INVALID_FILE_ATTRIBUTES)return;
    std::lock_guard g(s.lock);
    if(!s.f && (s.f=_wfopen(s.path.c_str(),L"wb"))){
        std::fputs("record_frame\tqueue_type\tcommand\tshader1\tshader2\tshader3\tx\ty\tz\tbegin_gpu\tend_gpu\tgpu_hz\n",s.f);
        s.first=f;
    }
}
TimedCommand PassTimes::measure(ID3D12Device* dev,ID3D12GraphicsCommandList* list,const char* label,ShaderKeys keys,UINT x,UINT y,UINT z){
    auto& s=sink();auto first=s.first.load(std::memory_order_relaxed),frame=s.frame.load(std::memory_order_relaxed);
    // Short bursts rather than timing every draw of the title.
    if(first==UINT64_MAX || (frame-first)%24>=3 || list->GetType()==D3D12_COMMAND_LIST_TYPE_COPY)return {};
    if(!s.focus.empty() && !s.focus.contains(keys.a) && !s.focus.contains(keys.b) && !s.focus.contains(keys.c))return {};
    if(!data)data=std::make_shared<TimedRecording>();
    if(data->chunks.empty() || data->chunks.back().entries.size()==capacity){
        Chunk c;if(!c.init(dev))return {};data->chunks.push_back(std::move(c));
    }
    auto& c=data->chunks.back();auto i=UINT(c.entries.size())*2;
    c.entries.push_back({frame,label,keys,x,y,z});
    list->EndQuery(c.heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i);
    return {list,c.heap.Get(),i+1};
}
void PassTimes::close(ID3D12GraphicsCommandList* list){
    if(data)for(auto& c:data->chunks)list->ResolveQueryData(c.heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,UINT(c.entries.size())*2,c.buf.Get(),0);
}
void PassTimes::save(const TimedRecording& r,UINT qt,UINT64 hz){
    auto& s=sink();std::lock_guard g(s.lock);if(!s.f || !hz)return;
    for(auto& c:r.chunks){
        UINT64* ts{};D3D12_RANGE range{0,c.entries.size()*16};
        if(FAILED(c.buf->Map(0,&range,reinterpret_cast<void**>(&ts))))continue;
        size_t j=0;for(auto& e:c.entries){
            std::fprintf(s.f,"%llu\t%u\t%s\t%016llx\t%016llx\t%016llx\t%u\t%u\t%u\t%llu\t%llu\t%llu\n",
                e.frame,qt,e.label,e.keys.a,e.keys.b,e.keys.c,e.x,e.y,e.z,ts[j],ts[j+1],hz);j+=2;
        }
        D3D12_RANGE none{};c.buf->Unmap(0,&none);
    }
    std::fflush(s.f);
}
