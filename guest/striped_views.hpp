#pragma once
#include "descriptor_pages.hpp"
#include <vector>
#include <utility>

// Independent command-recording workers normally copy from CPU descriptor
// pages into different visible pages. Keep an entire CopyDescriptors call
// atomic, locking its source banks shared and its destinations exclusive in
// one order. No pointer to mutable slot metadata survives its guard.
template<class View,class Lock> class StripedViews {
    using Addr=std::uintptr_t;
    using Mask=std::uint32_t;
    struct alignas(64) Bank{Lock lock;DescriptorPages<View,128> data;};
    std::array<Bank,32> banks_;
    unsigned stride_=1,shift_{};
    Addr slot(Addr a)const{return shift_<32?a>>shift_:a/stride_;}
    static unsigned page_bank(Addr n){return unsigned(n^(n>>6)^(n>>13)^(n>>21))&31;}
    unsigned bank(Addr a)const{return page_bank(slot(a)>>6);}
    unsigned run(Addr a)const{return 64-unsigned(slot(a)&63);}
public:
    unsigned stride()const{return stride_;}
    void increment(unsigned step){
        stride_=step;shift_=std::has_single_bit(step)?std::countr_zero(step):32;
        for(auto& b:banks_)b.data.increment(step);
    }
    Mask mask(Addr a)const{return Mask(1)<<bank(a);}
    Mask mask(Addr a,unsigned n)const{
        if(n>32*64)return ~Mask(0);
        Mask m=0;while(n){m|=mask(a);auto k=std::min(run(a),n);n-=k;a+=Addr(k)*stride_;}return m;
    }
    class Guard{
        StripedViews& store_;Mask all_,write_;
    public:
        Guard(StripedViews& store,Mask r,Mask w):store_(store),all_(r|w),write_(w){
            for(auto m=all_;m;m&=m-1){auto i=std::countr_zero(m);auto& l=store_.banks_[i].lock;
                if(w&(Mask(1)<<i))l.lock();else l.lock_shared();}
        }
        Guard(const Guard&)=delete;
        void unlock(){
            for(auto m=all_;m;m&=m-1){auto i=std::countr_zero(m);auto& l=store_.banks_[i].lock;
                if(write_&(Mask(1)<<i))l.unlock();else l.unlock_shared();}
            all_=0;
        }
        ~Guard(){unlock();}
    };
    Guard guard(Mask reads,Mask writes){return {*this,reads,writes};}
    const View* find(Addr a)const{return banks_[bank(a)].data.find(a);}
    bool write(Addr a,const View* v){return banks_[bank(a)].data.write(a,v);}
    bool copy(Addr s,Addr d,unsigned n){
        // A scatter-copy often consists of single descriptors from unrelated
        // heaps. Resolve both handles once; there is no range edge to split.
        if(n==1){
            auto a=slot(s),b=slot(d);auto si=unsigned(a&63),di=unsigned(b&63);
            return banks_[page_bank(b>>6)].data.copy_one_from(banks_[page_bank(a>>6)].data,
                    s-Addr(si)*stride_,si,d-Addr(di)*stride_,di);
        }
        bool changed=false;
        while(n){auto k=std::min({n,run(s),run(d)});auto& to=banks_[bank(d)].data;
            changed|=to.copy_from(banks_[bank(s)].data,s,d,k);
            n-=k;s+=Addr(k)*stride_;d+=Addr(k)*stride_;}
        return changed;
    }
    template<class Address>bool scatter(Addr dest,unsigned n,Address&& source){
        bool changed=false;unsigned first=0;
        while(first<n){
            auto d=slot(dest);auto di=unsigned(d&63);auto count=std::min(n-first,64-di);
            changed|=banks_[page_bank(d>>6)].data.scatter_page(dest-Addr(di)*stride_,di,count,[&](unsigned j){
                auto a=source(first+j),s=slot(a);
                return banks_[page_bank(s>>6)].data.copy_source(a,unsigned(s&63));
            });
            first+=count;dest+=Addr(count)*stride_;
        }
        return changed;
    }
    bool erase(Addr a,unsigned n){
        bool changed=false;while(n){auto k=std::min(run(a),n);changed|=banks_[bank(a)].data.erase(a,k);n-=k;a+=Addr(k)*stride_;}
        return changed;
    }
    bool possible(Addr lo,Addr hi)const{
        if(hi-lo>Addr(512)*stride_)return true;
        while(lo<hi){auto end=std::min(hi,lo+Addr(run(lo))*stride_);
            if(banks_[bank(lo)].data.possible(lo,end))return true;
            lo=end;}
        return false;
    }
    template<class F>void collect(Addr lo,Addr hi,F&& f){
        if(hi-lo>Addr(2048)*stride_){
            // Unbounded tables can span a million descriptors; retain the
            // sparse-page walk and descriptor order across the banks.
            using Use=decltype(View::upload);std::vector<std::pair<Addr,Use>> hits;
            for(auto& b:banks_)b.data.collect_at(lo,hi,[&](Addr a,const Use& u){hits.emplace_back(a,u);});
            std::sort(hits.begin(),hits.end(),[](auto& a,auto& b){return a.first<b.first;});
            for(auto& [_,u]:hits)f(u);
        }else while(lo<hi){auto end=std::min(hi,lo+Addr(run(lo))*stride_);
            banks_[bank(lo)].data.collect(lo,end,f);lo=end;}
    }
};
