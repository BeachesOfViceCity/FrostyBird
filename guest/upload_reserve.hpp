#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

// A small reserve of *unissued* committed upload buffers. Used resources
// never return to this reserve. Only exact, repeatedly successful requests
// can be prepared, using the same device, state, heap flags and node masks.
class UploadReserve {
    using Buffer=Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Kind{
        D3D12_HEAP_PROPERTIES heap{};D3D12_RESOURCE_DESC desc{};D3D12_HEAP_FLAGS flags{};
        D3D12_RESOURCE_STATES state{};unsigned seen{},count{};bool failed{};
        std::array<Buffer,3> ready;
        bool matches(const D3D12_HEAP_PROPERTIES& h,D3D12_HEAP_FLAGS f,const D3D12_RESOURCE_DESC& d,D3D12_RESOURCE_STATES s)const{
            return desc.Width==d.Width && desc.Alignment==d.Alignment && heap.CreationNodeMask==h.CreationNodeMask &&
                heap.VisibleNodeMask==h.VisibleNodeMask && flags==f && state==s;
        }
    };
    static constexpr UINT64 cap=128ull<<20;
    std::array<Kind,8> kinds_;unsigned used_{},cursor_{};UINT64 bytes_{};
    std::mutex lock_;std::condition_variable changed_;std::thread worker_;bool stop_{},disabled_{};
    static bool eligible(const D3D12_HEAP_PROPERTIES* h,D3D12_HEAP_FLAGS f,const D3D12_RESOURCE_DESC& d){
        return h && h->Type==D3D12_HEAP_TYPE_UPLOAD && h->CPUPageProperty==D3D12_CPU_PAGE_PROPERTY_UNKNOWN &&
            h->MemoryPoolPreference==D3D12_MEMORY_POOL_UNKNOWN && (f==D3D12_HEAP_FLAG_NONE || f==D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) &&
            d.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER && d.Layout==D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
            d.Flags==D3D12_RESOURCE_FLAG_NONE && d.Format==DXGI_FORMAT_UNKNOWN && d.Height==1 && d.DepthOrArraySize==1 &&
            d.MipLevels==1 && d.SampleDesc.Count==1 && !d.SampleDesc.Quality && std::has_single_bit(d.Width) && d.Width>=1u<<20 && d.Width<=32u<<20;
    }
    Kind* find(const D3D12_HEAP_PROPERTIES& h,D3D12_HEAP_FLAGS f,const D3D12_RESOURCE_DESC& d,D3D12_RESOURCE_STATES s){
        for(unsigned i=0;i<used_;++i)if(kinds_[i].matches(h,f,d,s))return &kinds_[i];
        return nullptr;
    }
    Kind* next(){
        for(unsigned n=0;!disabled_ && n<used_;++n){
            auto i=(cursor_+n)%used_;auto& k=kinds_[i];
            if(!k.failed && k.seen>=2 && k.count<(k.desc.Width>=(16u<<20)?2:3) && k.desc.Width<=cap-bytes_){
                cursor_=(i+1)%used_;return &k;
            }
        }
        return nullptr;
    }
    void work(ID3D12Device* dev){
        std::unique_lock g(lock_);
        while(!stop_){
            auto k=next();if(!k){changed_.wait(g);continue;}
            bytes_+=k->desc.Width;g.unlock();
            Buffer r;auto hr=dev->CreateCommittedResource(&k->heap,k->flags,&k->desc,k->state,nullptr,IID_PPV_ARGS(&r));
            g.lock();
            if(FAILED(hr) || stop_ || disabled_){bytes_-=k->desc.Width;k->failed=true;}
            else k->ready[k->count++]=std::move(r);
        }
    }
public:
    UploadReserve(){char value[8]{};enabled=GetEnvironmentVariableA("FROSTY_UPLOAD_RESERVE",value,sizeof(value))==1 && value[0]=='1';}
    ~UploadReserve(){
        {std::lock_guard g(lock_);stop_=true;}changed_.notify_one();if(worker_.joinable())worker_.join();
    }
    bool enabled{};
    HRESULT create(ID3D12Device* dev,const D3D12_HEAP_PROPERTIES* h,D3D12_HEAP_FLAGS f,const D3D12_RESOURCE_DESC& d,
            D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID id,void** out,bool& hit,bool& candidate){
        hit=false;candidate=enabled && out && !clear && id==__uuidof(ID3D12Resource) && eligible(h,f,d);
        if(candidate){
            std::lock_guard g(lock_);
            if(auto k=find(*h,f,d,state);k && k->count){
                *out=k->ready[--k->count].Detach();bytes_-=k->desc.Width;hit=true;changed_.notify_one();return S_OK;
            }
        }
        auto hr=dev->CreateCommittedResource(h,f,&d,state,clear,id,out);
        if(hr==E_OUTOFMEMORY && enabled){
            // Give normal demand priority. Release unopened spares on memory
            // pressure and stop speculative allocations for this device.
            std::unique_lock g(lock_);disabled_=true;
            for(unsigned i=0;i<used_;++i)while(kinds_[i].count){
                bytes_-=kinds_[i].desc.Width;kinds_[i].ready[--kinds_[i].count].Reset();}
            g.unlock();changed_.notify_one();hr=dev->CreateCommittedResource(h,f,&d,state,clear,id,out);
        }
        if(hr==S_OK && candidate && *out){
            std::lock_guard g(lock_);if(disabled_)return hr;
            auto k=find(*h,f,d,state);
            if(!k && used_<kinds_.size()){k=&kinds_[used_++];k->heap=*h;k->desc=d;k->flags=f;k->state=state;}
            if(k && k->seen<2 && ++k->seen==2){
                if(!worker_.joinable())try{worker_=std::thread([this,dev]{work(dev);});}
                catch(const std::system_error&){disabled_=true;}catch(const std::bad_alloc&){disabled_=true;}
                changed_.notify_one();
            }
        }
        return hr;
    }
};
