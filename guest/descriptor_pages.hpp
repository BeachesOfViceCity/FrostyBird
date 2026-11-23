#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

// CPU descriptor handles need not be aligned to the descriptor increment.
// Preserve their residue in the page key. Copies visit one page per run,
// and range binds visit only pages containing a pending placed image.
template<class View,unsigned Buckets=4096> class DescriptorPages {
    static_assert(std::has_single_bit(Buckets));
    using Addr=std::uintptr_t;
    using Bits=std::uint64_t;
    struct Page{Bits valid{},pending{};std::array<View,64> slot{};};
    // Keep only keys and pointers in the probe sequence, not 64 descriptors
    // in every hash node. Page addresses remain stable across growth.
    struct Cell{Addr key{};std::unique_ptr<Page> page;};
    std::vector<Cell> dir_;size_t used_{};
    static size_t home(Addr k,size_t mask){
        auto h=(std::uint64_t(k)^(std::uint64_t(k)>>13))*0x9e3779b97f4a7c15ull;
        return size_t(h^(h>>32))&mask;
    }
    static size_t probe(const std::vector<Cell>& d,Addr k){
        auto m=d.size()-1,i=home(k,m);while(d[i].page && d[i].key!=k)i=(i+1)&m;return i;
    }
    struct Hot{Addr key{};Page* page{};};
    std::array<Hot,64> hot_{};
    std::map<Addr,Page*> pending_;
    unsigned step_=1,shift_{};
    // A collision can only send a bind through the locked path. Keep counts,
    // not sticky bits, as different heaps (including different residues)
    // can be live in the same bucket. Writers hold the device metadata lock.
    std::array<std::atomic<unsigned>,Buckets> pending_pages_{};
    Addr page_hash(Addr a)const{auto n=(shift_<32?a>>shift_:a/step_)>>6;return n^(n>>12)^(n>>24);}
    auto& bucket(this auto& self,Addr a){
        return self.pending_pages_[self.page_hash(a)&(Buckets-1)];
    }
    unsigned index(Addr a)const{return unsigned((shift_<32?a>>shift_:a/step_)&63);}
    Addr base(Addr a,unsigned i)const{return a-Addr(i)*step_;}
    // Only writers update the cache. A source shared by parallel copies can
    // read a hit; a rehash never invalidates the mapped object itself.
    const Page* peek(Addr k)const{
        auto& h=hot_[page_hash(k)&63];if(h.key==k)return h.page;
        return dir_.empty()?nullptr:dir_[probe(dir_,k)].page.get();
    }
    Page* page(Addr k){auto p=const_cast<Page*>(peek(k));hot_[page_hash(k)&63]={k,p};return p;}
    Page* ensure(Addr k){
        if(4*used_>=3*dir_.size()){
            std::vector<Cell> fresh(std::max<size_t>(16,dir_.size()*2));
            for(auto& c:dir_)if(c.page){auto i=probe(fresh,c.key);fresh[i]=std::move(c);}
            dir_.swap(fresh);
        }
        auto& c=dir_[probe(dir_,k)];
        if(!c.page){c.page=std::make_unique<Page>();c.key=k;++used_;}
        auto p=c.page.get();hot_[page_hash(k)&63]={k,p};return p;
    }
    void drop(Addr k){
        hot_[page_hash(k)&63]={k,nullptr};auto hole=probe(dir_,k),m=dir_.size()-1;
        dir_[hole].page.reset();--used_;
        for(auto j=(hole+1)&m;dir_[j].page;j=(j+1)&m){
            auto first=home(dir_[j].key,m);
            if(((hole-first)&m)<((j-first)&m)){dir_[hole]=std::move(dir_[j]);hole=j;}
        }
    }
    void sync_page(Addr base,Page& p,Bits old){
        if(!old && p.pending){pending_.emplace(base,&p);bucket(base).fetch_add(1,std::memory_order_release);}
        else if(old && !p.pending){pending_.erase(base);bucket(base).fetch_sub(1,std::memory_order_release);}
    }
    static bool put(Page& p,unsigned n,const View* v){
        Bits bit=Bits(1)<<n;auto& slot=p.slot[n];auto* old=(p.pending&bit)?slot.upload.get():nullptr;
        if(v){
            if(!(p.valid&bit) || slot.generation!=v->generation)slot=*v;
            p.valid|=bit;
        }else if(p.valid&bit){slot={};p.valid&=~bit;}
        if(slot.upload && slot.upload->access->settled.load())slot.upload.reset();
        auto* now=slot.upload.get();if(now)p.pending|=bit;else p.pending&=~bit;
        return old!=now;
    }
    struct Ref{const View* view;bool pending;};
    Ref source(Addr k,unsigned i)const{
        auto p=peek(k);auto bit=Bits(1)<<i;
        return p && (p->valid&bit)?Ref{&p->slot[i],bool(p->pending&bit)}:Ref{};
    }
    static bool copy_slot(Page& dst,unsigned i,Ref from){
        auto bit=Bits(1)<<i;
        if(from.pending || (dst.pending&bit))return put(dst,i,from.view);
        if(auto v=from.view){
            auto& to=dst.slot[i];
            if(!(dst.valid&bit) || to.generation!=v->generation){
                if constexpr(requires{to.copy_settled(*v);})to.copy_settled(*v);else to=*v;
            }
            dst.valid|=bit;
        }else dst.valid&=~bit;
        return false;
    }
public:
    void increment(unsigned stride){step_=stride;shift_=std::has_single_bit(stride)?std::countr_zero(stride):32;}
    bool possible(Addr lo,Addr hi)const{
        if(lo>=hi)return false;
        auto key=base(lo,index(lo)),span=Addr(64)*step_;
        if(hi-key>8*span)return true;
        for(;;){
            if(bucket(key).load(std::memory_order_acquire))return true;
            if(hi-key<=span)return false;
            key+=span;
        }
    }
    const View* find(Addr a)const{
        auto i=index(a);auto p=peek(base(a,i));return p && ((p->valid>>i)&1u)?&p->slot[i]:nullptr;
    }
    bool write(Addr a,const View* v){
        auto i=index(a);auto key=base(a,i);auto* p=page(key);
        if(!p){if(!v)return false;p=ensure(key);}
        auto before=p->pending;bool changed=put(*p,i,v);sync_page(key,*p,before);return changed;
    }
    bool copy(Addr s,Addr d,unsigned count){return copy_from(*this,s,d,count);}
    bool copy_one_from(const DescriptorPages& from,Addr sk,unsigned si,Addr dk,unsigned di){
        auto src=from.source(sk,si);auto* dst=page(dk);
        if(!dst){if(!src.view)return false;dst=ensure(dk);}
        auto old=dst->pending;auto changed=copy_slot(*dst,di,src);
        if(changed)sync_page(dk,*dst,old);
        return changed;
    }
    auto copy_source(Addr a,unsigned i)const{return source(a-Addr(i)*step_,i);}
    template<class Lookup>bool scatter_page(Addr dk,unsigned di,unsigned count,Lookup&& get){
        auto* dst=page(dk);auto before=dst?dst->pending:0;bool changed=false;
        for(unsigned j=0;j<count;++j){
            auto v=get(j);if(!dst){if(!v.view)continue;dst=ensure(dk);}
            changed|=copy_slot(*dst,di+j,v);
        }
        if(changed)sync_page(dk,*dst,before);
        return changed;
    }
    bool copy_from(const DescriptorPages& from,Addr s,Addr d,unsigned count){
        bool changed=false;
        while(count){
            auto si=index(s),di=index(d);auto sk=base(s,si),dk=base(d,di);
            auto n=std::min({count,64-si,64-di});auto* src=from.peek(sk);auto* dst=page(dk);
            Bits mask=n==64?~Bits(0):(Bits(1)<<n)-1,used=src?(src->valid>>si)&mask:0;
            if(used && !dst)dst=ensure(dk);
            if(dst){
                auto old=(dst->valid>>di)&mask;
                if((used || old) && (!src || src!=dst || si==di || si+n<=di || di+n<=si) &&
                   !(((src?src->pending>>si:0)|(dst->pending>>di))&mask)){
                    for(auto bits=used;bits;bits&=bits-1){
                        auto j=unsigned(std::countr_zero(bits));auto& v=src->slot[si+j];auto& to=dst->slot[di+j];
                        if(!(old&(Bits(1)<<j)) || to.generation!=v.generation){
                            if constexpr(requires{to.copy_settled(v);})to.copy_settled(v);else to=v;
                        }
                    }
                    dst->valid=(dst->valid&~(mask<<di))|(used<<di);
                }else if(used || old){auto before=dst->pending;
                    for(unsigned j=0;j<n;++j)changed|=put(*dst,di+j,(used>>j)&1?&src->slot[si+j]:nullptr);
                    sync_page(dk,*dst,before);}
            }
            s+=Addr(n)*step_;d+=Addr(n)*step_;count-=n;
        }
        return changed;
    }
    bool erase(Addr a,unsigned count){
        bool changed=false;
        while(count){auto i=index(a);auto key=base(a,i);auto n=std::min(count,64-i);
            if(auto* p=page(key)){
                if(!i && n==64){if(p->pending){changed=true;pending_.erase(key);bucket(key).fetch_sub(1,std::memory_order_release);}drop(key);}
                else{auto before=p->pending;for(unsigned j=0;j<n;++j)changed|=put(*p,i+j,nullptr);
                    sync_page(key,*p,before);if(!p->valid)drop(key);}
            }
            a+=Addr(n)*step_;count-=n;
        }
        return changed;
    }
    template<class F>void collect(Addr lo,Addr hi,F&& f){collect_at(lo,hi,[&](Addr,const auto& v){f(v);});}
    template<class F>void collect_at(Addr lo,Addr hi,F&& f){
        if(lo>=hi || pending_.empty())return;
        auto i=index(lo);auto it=pending_.lower_bound(base(lo,i));auto residue=lo%step_;
        while(it!=pending_.end() && it->first<hi){
            auto [key,p]=*it;
            if(key%step_!=residue){++it;continue;}
            auto bits=p->pending;
            if(key<lo)bits&=~Bits(0)<<i;
            while(bits){auto b=std::countr_zero(bits);auto a=key+Addr(b)*step_;if(a>=hi)break;
                bits&=bits-1;auto& use=p->slot[b].upload;
                if(use->access->settled.load()){p->pending&=~(Bits(1)<<b);use.reset();}
                else f(a,use);
            }
            if(!p->pending){bucket(key).fetch_sub(1,std::memory_order_release);it=pending_.erase(it);}else ++it;
        }
    }
};
