// Optional indirect argument counters for developer runs. Never copy pixels.
#define NOMINMAX
#include "pass_capture.hpp"
#include <cstdio>
#include <charconv>
#include <string_view>
#include <cstring>
PassCapture::PassCapture(){
    wchar_t dir[1024]{};
    auto n=GetEnvironmentVariableW(L"FROSTY_CAPTURE_DIR",dir,1024);
    if(!n||n>=1024)return;
    auto parse=[](const char* name,std::set<UINT64>& selected){
        char keys[2048]{};auto k=GetEnvironmentVariableA(name,keys,sizeof(keys));if(k>=sizeof(keys))return;
        std::string_view s(keys,k);
        while(!s.empty()){auto pos=s.find(',');auto token=s.substr(0,pos);UINT64 key{};
            auto [p,ec]=std::from_chars(token.data(),token.data()+token.size(),key,16);
            if(ec!=std::errc{} || p!=token.data()+token.size()){selected.clear();return;}
            selected.insert(key);if(pos==s.npos)break;s.remove_prefix(pos+1);}
    };
    parse("FROSTY_CAPTURE_INDIRECT",indirect_);
    dir_=dir;
}
void PassCapture::arguments(ID3D12Device* dev,ID3D12GraphicsCommandList* list,UINT64 shader,D3D12_INDIRECT_ARGUMENT_TYPE kind,UINT count,
                            ID3D12Resource* args,UINT64 offset,ID3D12Resource* counter,UINT64 at,std::vector<PassReadback>& out){
    if(!indirect_.contains(shader) || !count || !args)return;
    UINT words=kind==D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH?3:kind==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW?4:kind==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED?5:0;
    auto a=args->GetDesc();if(!words || a.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || offset>a.Width || 4*words>a.Width-offset)return;
    if(counter){auto c=counter->GetDesc();if(c.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || at>c.Width || 4>c.Width-at)return;}
    PassReadback r{};r.frame=frame_.load();r.shader=shader;r.arg_kind=UINT(kind);r.arg_words=words;r.max_count=count;r.source=args;r.counter=counter;
    {std::lock_guard g(lock_);auto& samples=arg_samples_[{shader,r.frame/90}];if(samples>=3)return;++samples;}
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER,0,24,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
    if(FAILED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r.buffer))))return;
    auto copy=[&](ID3D12Resource* src,UINT64 so,UINT dst,UINT bytes){
        D3D12_HEAP_PROPERTIES h{};D3D12_HEAP_FLAGS flags{};auto hr=src->GetHeapProperties(&h,&flags);
        bool trans=FAILED(hr)||h.Type!=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_BARRIER b{};b.Transition={src,0,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_COPY_SOURCE};
        if(trans)list->ResourceBarrier(1,&b);
        list->CopyBufferRegion(r.buffer.Get(),dst,src,so,bytes);
        if(trans){std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);}
    };
    copy(args,offset,0,words*4);if(counter)copy(counter,at,20,4);
    out.push_back(std::move(r));
}
void PassCapture::save(std::vector<PassReadback>& buffers){
    for(auto& r:buffers){
        BYTE* src{};if(FAILED(r.buffer->Map(0,nullptr,(void**)&src)))continue;
        if(r.arg_words){
            std::lock_guard g(lock_);
            if(auto f=_wfopen((dir_+L"\\gpu-indirect.tsv").c_str(),L"ab")){
                _fseeki64(f,0,SEEK_END);
                if(!std::ftell(f))std::fputs("presented\tshader\tkind\tmax_count\tcount_buffer\tfirst_record\n",f);
                UINT words[6]{};std::memcpy(words,src,r.arg_words*4);if(r.counter)std::memcpy(words+5,src+20,4);
                std::fprintf(f,"%llu\t%016llx\t%u\t%u\t",r.frame,r.shader,r.arg_kind,r.max_count);
                if(r.counter)std::fprintf(f,"%u\t",words[5]);else std::fputs("-\t",f);
                for(UINT i=0;i<r.arg_words;++i)std::fprintf(f,"%s%u",i?",":"",words[i]);
                std::fputc('\n',f);std::fclose(f);
            }
        }
        D3D12_RANGE empty{};r.buffer->Unmap(0,&empty);
    }
    buffers.clear();
}
