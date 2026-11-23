// Read untouched and recycled DEFAULT allocations from both queue types.
// Every read-before-write is paired with a nonzero GPU write/read control.
#define NOMINMAX
#include <d3d12.h>
#include <wrl/client.h>
#include "upload_reserve.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using R=ComPtr<ID3D12Resource>;
static void ok(HRESULT hr){if(FAILED(hr)){std::fprintf(stderr,"zero-check HRESULT %#lx\n",(unsigned long)hr);throw std::runtime_error("GPU call");}}
int main()try{
    ComPtr<ID3D12Device> d;ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
    auto create=[&](D3D12_HEAP_TYPE type,const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;R r;
        ok(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto buf=[&](D3D12_HEAP_TYPE type,UINT64 size,D3D12_RESOURCE_STATES state){
        return create(type,{D3D12_RESOURCE_DIMENSION_BUFFER,0,size,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE},state);};
    constexpr UINT64 big=1ull<<20;
    constexpr std::array<UINT64,9> sizes{65536,big/4,big,4*big,5*big,65536,3*big,big/2,7*big};
    std::array<ComPtr<ID3D12CommandQueue>,2> queue;ComPtr<ID3D12Fence> fence;ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    const D3D12_COMMAND_LIST_TYPE types[]{D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_TYPE_COMPUTE};
    for(UINT i=0;i<2;++i){D3D12_COMMAND_QUEUE_DESC q{};q.Type=types[i];ok(d->CreateCommandQueue(&q,IID_PPV_ARGS(&queue[i])));}
    UINT64 seq=0,checked=0;UINT overlaps=0;std::map<UINT64,UINT64> past;
    for(bool image:{false,true}){
        std::array<R,sizes.size()> objects;std::array<UINT64,sizes.size()> loc{},extent{};std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,sizes.size()> fp{};
        std::array<D3D12_RESOURCE_DESC,sizes.size()> descs{};UINT64 total=0;
        for(UINT i=0;i<sizes.size();++i){
            auto& desc=descs[i];desc={D3D12_RESOURCE_DIMENSION_BUFFER,0,sizes[i],1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
            if(image){desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=128*(1+i%3);desc.Height=96+4*i;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
                UINT rows{};UINT64 pitch{};d->GetCopyableFootprints(&desc,0,1,0,&fp[i],&rows,&pitch,&extent[i]);
                if(fp[i].Footprint.RowPitch!=pitch || rows!=desc.Height)return 2;
            }else extent[i]=sizes[i];
            loc[i]=total;total=(total+extent[i]+511)&~UINT64(511);
        }
        auto read=buf(D3D12_HEAP_TYPE_READBACK,2*total,D3D12_RESOURCE_STATE_COPY_DEST);
        auto data=buf(D3D12_HEAP_TYPE_UPLOAD,*std::max_element(extent.begin(),extent.end()),D3D12_RESOURCE_STATE_GENERIC_READ);
        void* u{};ok(data->Map(0,nullptr,&u));std::memset(u,0xa7,SIZE_T(data->GetDesc().Width));data->Unmap(0,nullptr);
        for(UINT round=0;round<6;++round){
            auto qi=round&1;ComPtr<ID3D12CommandAllocator> a;ok(d->CreateCommandAllocator(types[qi],IID_PPV_ARGS(&a)));
            ComPtr<ID3D12GraphicsCommandList> l;ok(d->CreateCommandList(0,types[qi],a.Get(),nullptr,IID_PPV_ARGS(&l)));
            std::vector<UINT> fresh;
            for(UINT i=0;i<sizes.size();++i){
                if(objects[i])continue;
                objects[i]=create(D3D12_HEAP_TYPE_DEFAULT,descs[i],D3D12_RESOURCE_STATE_COPY_SOURCE);auto r=objects[i].Get();fresh.push_back(i);
                if(!image){auto va=r->GetGPUVirtualAddress();auto it=past.upper_bound(va);
                    if(it!=past.begin()){--it;overlaps+=va-it->first<it->second;}
                    past[va]=sizes[i];}
                auto copy=[&](bool to_host,UINT64 off){
                    if(!image){if(to_host)l->CopyBufferRegion(read.Get(),off,r,0,extent[i]);else l->CopyBufferRegion(r,0,data.Get(),0,extent[i]);}
                    else{D3D12_TEXTURE_COPY_LOCATION tex{},linear{};tex.pResource=r;linear.pResource=to_host?read.Get():data.Get();
                        linear.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;linear.PlacedFootprint=fp[i];linear.PlacedFootprint.Offset=off;
                        l->CopyTextureRegion(to_host?&linear:&tex,0,0,0,to_host?&tex:&linear,nullptr);}
                };
                copy(true,loc[i]);D3D12_RESOURCE_BARRIER t{};t.Transition={r,0,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
                l->ResourceBarrier(1,&t);copy(false,0);std::swap(t.Transition.StateBefore,t.Transition.StateAfter);l->ResourceBarrier(1,&t);copy(true,total+loc[i]);
            }
            ok(l->Close());ID3D12CommandList* ls[]{l.Get()};queue[qi]->ExecuteCommandLists(1,ls);ok(queue[qi]->Signal(fence.Get(),++seq));ok(fence->SetEventOnCompletion(seq,nullptr));
            UINT* words{};ok(read->Map(0,nullptr,reinterpret_cast<void**>(&words)));
            for(UINT i:fresh)for(UINT64 j=0;j<extent[i]/4;++j){auto k=loc[i]/4+j;
                if(words[k] || words[k+total/4]!=0xa7a7a7a7u){std::fprintf(stderr,"%s round %u resource %u word %llu: %#x / %#x\n",image?"image":"buffer",round,i,j,words[k],words[k+total/4]);return 4;}
                ++checked;}
            D3D12_RANGE none{};read->Unmap(0,&none);
            for(UINT i=round&1;i<sizes.size();i+=2)objects[i].Reset();
        }
    }
    // More outstanding generations than the backend's command buffer ring.
    // Alternate the first reader, then read its write on the other queue.
    // Keep the recordings live through their GPU fences rather than waiting
    // on the CPU between each allocation and submission.
    constexpr UINT rounds=64,span=65536,words_per=span/sizeof(UINT);
    auto rb=buf(D3D12_HEAP_TYPE_READBACK,2*rounds*span,D3D12_RESOURCE_STATE_COPY_DEST);
    auto upload=buf(D3D12_HEAP_TYPE_UPLOAD,rounds*span,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* ptr{};ok(upload->Map(0,nullptr,&ptr));
    for(UINT i=0;i<rounds*words_per;i++)static_cast<UINT*>(ptr)[i]=0x7351b8e9u+i;
    upload->Unmap(0,nullptr);
    ok(rb->Map(0,nullptr,&ptr));std::memset(ptr,0x5e,2*rounds*span);rb->Unmap(0,nullptr);
    std::array<ComPtr<ID3D12Fence>,2> fences;UINT64 tail[2]{};
    for(auto& f:fences)ok(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));
    struct Recording{R resource;ComPtr<ID3D12CommandAllocator> alloc[2];ComPtr<ID3D12GraphicsCommandList> list[2];};
    std::array<Recording,rounds> records;
    for(UINT i=0;i<rounds;++i){
        auto& rec=records[i];auto w=i&1,other=w^1;
        rec.resource=buf(D3D12_HEAP_TYPE_DEFAULT,span,D3D12_RESOURCE_STATE_COPY_SOURCE);
        // Free one still-pending neighbor without invalidating a sealed batch.
        buf(D3D12_HEAP_TYPE_DEFAULT,span,D3D12_RESOURCE_STATE_COPY_SOURCE).Reset();
        for(UINT q=0;q<2;++q){
            ok(d->CreateCommandAllocator(types[q],IID_PPV_ARGS(&rec.alloc[q])));
            ok(d->CreateCommandList(0,types[q],rec.alloc[q].Get(),nullptr,IID_PPV_ARGS(&rec.list[q])));
        }
        auto* l=rec.list[w].Get();auto* r=rec.resource.Get();
        l->CopyBufferRegion(rb.Get(),i*span,r,0,span);
        D3D12_RESOURCE_BARRIER t{};t.Transition={r,0,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
        l->ResourceBarrier(1,&t);l->CopyBufferRegion(r,0,upload.Get(),i*span,span);
        std::swap(t.Transition.StateBefore,t.Transition.StateAfter);l->ResourceBarrier(1,&t);ok(l->Close());
        rec.list[other]->CopyBufferRegion(rb.Get(),(rounds+i)*span,r,0,span);ok(rec.list[other]->Close());
        ID3D12CommandList* first[]{l};queue[w]->ExecuteCommandLists(1,first);
        ok(queue[w]->Signal(fences[w].Get(),++tail[w]));ok(queue[other]->Wait(fences[w].Get(),tail[w]));
        ID3D12CommandList* second[]{rec.list[other].Get()};queue[other]->ExecuteCommandLists(1,second);
        ok(queue[other]->Signal(fences[other].Get(),++tail[other]));
    }
    for(UINT q=0;q<2;++q)ok(fences[q]->SetEventOnCompletion(tail[q],nullptr));
    ok(rb->Map(0,nullptr,&ptr));auto* words=static_cast<const UINT*>(ptr);
    for(UINT i=0;i<rounds*words_per;++i)if(words[i] || words[i+rounds*words_per]!=0x7351b8e9u+i){
        std::fprintf(stderr,"cross-queue generation %u word %u: %#x / %#x\n",i/words_per,i%words_per,words[i],words[i+rounds*words_per]);return 6;}
    D3D12_RANGE none{};rb->Unmap(0,&none);
    std::printf("DEFAULT buffers and color images: %llu initially zero words with nonzero copy controls, %u reused buffer ranges, both queues; %u cross-queue generations, %u ordered words\n",
        checked,overlaps,rounds,rounds*words_per);
    for(UINT pass=0;pass<6;++pass){
        auto qi=pass&1;std::array<ID3D12CommandList*,rounds> lists;
        for(UINT i=0;i<rounds;++i){
            auto& r=records[i];auto* a=r.alloc[qi].Get();auto* l=r.list[qi].Get();
            ok(a->Reset());ok(l->Reset(a,nullptr));
            if(!i && !pass && SUCCEEDED(a->Reset()))return 13; // open recording must still prevent reset
            l->CopyBufferRegion(rb.Get(),i*span,upload.Get(),((i+pass+1)%rounds)*span,span);ok(l->Close());lists[i]=l;
        }
        queue[qi]->ExecuteCommandLists(rounds,lists.data());ok(queue[qi]->Signal(fence.Get(),++seq));ok(fence->SetEventOnCompletion(seq,nullptr));
        ok(rb->Map(0,nullptr,&ptr));words=static_cast<const UINT*>(ptr);
        for(UINT i=0;i<rounds*words_per;++i)
            if(words[i]!=0x7351b8e9u+((i+(pass+1)*words_per)%(rounds*words_per)))return 14;
        rb->Unmap(0,&none);
    }
    std::printf("Retired allocator reset: %u rerecordings, %u copy words verified on direct and compute; open-list reset rejected\n",6*rounds,6*rounds*words_per);
    UploadReserve reserve;
    if(reserve.enabled){
        constexpr UINT bytes=4u<<20,words=bytes/4;
        D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_UPLOAD;h.CreationNodeMask=h.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,0,bytes,1,1,1,DXGI_FORMAT_UNKNOWN,{1,0},D3D12_TEXTURE_LAYOUT_ROW_MAJOR,D3D12_RESOURCE_FLAG_NONE};
        auto out=buf(D3D12_HEAP_TYPE_READBACK,bytes,D3D12_RESOURCE_STATE_COPY_DEST);R prior;
        unsigned ready=0;bool hit,eligible;
        for(UINT k=0;k<24;++k){R u;ok(reserve.create(d.Get(),&h,D3D12_HEAP_FLAG_NONE,desc,D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(u.GetAddressOf()),hit,eligible));
            if(!eligible || (prior && u->GetGPUVirtualAddress()==prior->GetGPUVirtualAddress()))return 7;
            ready+=hit;D3D12_HEAP_PROPERTIES actual{};D3D12_HEAP_FLAGS flags{};ok(u->GetHeapProperties(&actual,&flags));
            if(u->GetDesc().Width!=bytes || actual.Type!=h.Type || flags!=D3D12_HEAP_FLAG_NONE)return 8;
            UINT* data{};ok(u->Map(0,nullptr,reinterpret_cast<void**>(&data)));
            for(UINT j=0;j<words;++j){if(data[j])return 9;data[j]=(k*words+j)^0xaa75dc11u;}u->Unmap(0,nullptr);
            auto qi=k&1;ComPtr<ID3D12CommandAllocator> ca;ok(d->CreateCommandAllocator(types[qi],IID_PPV_ARGS(&ca)));
            ComPtr<ID3D12GraphicsCommandList> cl;ok(d->CreateCommandList(0,types[qi],ca.Get(),nullptr,IID_PPV_ARGS(&cl)));
            cl->CopyBufferRegion(out.Get(),0,u.Get(),0,bytes);ok(cl->Close());ID3D12CommandList* lists[]{cl.Get()};queue[qi]->ExecuteCommandLists(1,lists);
            ok(queue[qi]->Signal(fence.Get(),++seq));ok(fence->SetEventOnCompletion(seq,nullptr));
            ok(out->Map(0,nullptr,reinterpret_cast<void**>(&data)));for(UINT j=0;j<words;++j)if(data[j]!=((k*words+j)^0xaa75dc11u))return 10;
            out->Unmap(0,&none);prior=std::move(u);
        }
        // A different size is a distinct template. An invalid description is
        // still rejected by the device, even with ready spares of this width.
        R u;desc.Width=2u<<20;ok(reserve.create(d.Get(),&h,D3D12_HEAP_FLAG_NONE,desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,
                __uuidof(ID3D12Resource),reinterpret_cast<void**>(u.GetAddressOf()),hit,eligible));
        if(hit || !eligible || u->GetDesc().Width!=desc.Width)return 11;
        u.Reset();desc.Width=bytes;desc.Height=0;
        if(SUCCEEDED(reserve.create(d.Get(),&h,D3D12_HEAP_FLAG_NONE,desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,
                __uuidof(ID3D12Resource),reinterpret_cast<void**>(u.GetAddressOf()),hit,eligible)) || eligible || u || !ready)return 12;
        std::printf("Untouched committed upload buffers: %u/%u words initially zero and copied on both queues, %u ready hits; size/invalid controls passed\n",
                24*words,24*words,ready);
    }
    return overlaps?0:5;
}catch(const std::exception& e){std::fprintf(stderr,"zero-check: %s\n",e.what());return 1;}
