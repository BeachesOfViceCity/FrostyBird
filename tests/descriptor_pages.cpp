// Compare copied/overwritten descriptor ranges to a flat reference table.
// Exercise shared images retiring between binds, partial heap destruction,
// and heaps with different handle residues on the same metadata page.
#include "../guest/descriptor_pages.hpp"
#include "../guest/striped_views.hpp"
#include <atomic>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <shared_mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
struct SharedLock{
    SRWLOCK lock_=SRWLOCK_INIT;
    void lock(){AcquireSRWLockExclusive(&lock_);}void unlock(){ReleaseSRWLockExclusive(&lock_);}
    void lock_shared(){AcquireSRWLockShared(&lock_);}void unlock_shared(){ReleaseSRWLockShared(&lock_);}
};
#else
using SharedLock=std::shared_mutex;
#endif
struct Access{std::atomic<bool> settled{};};
struct Use{std::shared_ptr<Access> access;unsigned id;};
struct View{std::uint64_t generation{};std::shared_ptr<Use> upload;};
int main(){
    std::mt19937 rng(7759);size_t checked=0;
    for(unsigned step:{16u,32u,48u,64u}){
        DescriptorPages<View> pages;pages.increment(step);
        StripedViews<View,SharedLock> singles;singles.increment(step);
        // Two different page keys share a summary bucket. Retiring the first
        // must not make the second invisible, including after a full erase.
        auto a=std::uintptr_t(step)*64*64+1,b=std::uintptr_t(step)*64*4161+5;
        auto live=std::make_shared<Use>(Use{std::make_shared<Access>(),1});
        View v{1,live};pages.write(a,&v);pages.write(b,&v);pages.erase(a,64);
        if(!pages.possible(b,b+step))throw std::runtime_error("collision lost active page");
        live->access->settled=true;pages.collect(b,b+step,[](auto&){throw std::runtime_error("settled page");});
        if(pages.possible(a,a+step) || pages.possible(b,b+step))throw std::runtime_error("stale summary bucket");
        pages.erase(b,64);
        std::array<std::vector<View>,3> model;for(auto& m:model)m.resize(341);
        auto addr=[&](unsigned h,unsigned i)->std::uintptr_t{return 0x12005u+h*8191u+i*step;};
        std::vector<std::shared_ptr<Use>> uses;
        for(unsigned j=0;j<40;++j)uses.push_back(std::make_shared<Use>(Use{std::make_shared<Access>(),j+1}));
        for(unsigned j=1;j<7000;++j){
            unsigned h=rng()%3,k=rng()%3;if(h==k)k=(k+1)%3;
            unsigned at=rng()%320,src=rng()%320,len=std::min({1u+unsigned(rng()%90),341-at,341-src});
            switch(rng()%7){
            case 0:case 1:case 2:{auto& v=model[h][at];v={j,rng()%3?uses[rng()%uses.size()]:nullptr};pages.write(addr(h,at),&v);singles.write(addr(h,at),&v);break;}
            case 3:case 4:pages.copy(addr(k,src),addr(h,at),len);
                for(unsigned n=0;n<len;++n)singles.copy(addr(k,src+n),addr(h,at+n),1);
                std::copy_n(model[k].begin()+src,len,model[h].begin()+at);break;
            case 5:pages.erase(addr(h,at),len);singles.erase(addr(h,at),len);std::fill_n(model[h].begin()+at,len,View{});break;
            default:if(!(j%53))uses[rng()%uses.size()]->access->settled=true;
            }
            std::vector<unsigned> want,got,one;
            for(unsigned i=at;i<at+len;++i){const auto& v=model[h][i];auto p=pages.find(addr(h,i));
                if((p?p->generation:0)!=v.generation)throw std::runtime_error("stale or missing descriptor");
                auto s=singles.find(addr(h,i));if((s?s->generation:0)!=v.generation)throw std::runtime_error("wrong single-slot copy");
                if(v.upload&&!v.upload->access->settled)want.push_back(v.upload->id);
                ++checked;}
            if(pages.possible(addr(h,at),addr(h,at+len)))
                pages.collect(addr(h,at),addr(h,at+len),[&](const auto& u){got.push_back(u->id);});
            if(singles.possible(addr(h,at),addr(h,at+len)))
                singles.collect(addr(h,at),addr(h,at+len),[&](const auto& u){one.push_back(u->id);});
            if(got!=want || one!=want)throw std::runtime_error("wrong pending subresource range");
        }
    }
    std::printf("Descriptor range and pending-image checks: %zu slots matched\n",checked);
    {
        DescriptorPages<View> grown;grown.increment(32);
        for(unsigned j=0;j<1024;++j){View x{j+1,{}};grown.write(5+0x10000ull*j,&x);}
        for(unsigned j=1;j<1024;j+=3)grown.erase(5+0x10000ull*j,64);
        for(unsigned j=0;j<1024;++j){auto p=grown.find(5+0x10000ull*j);
            if((p?p->generation:0)!=(j%3==1?0:j+1))throw std::runtime_error("lost page after probe-chain erase");}
    }
    StripedViews<View,SharedLock> views;views.increment(32);
    constexpr unsigned length=85;constexpr std::uintptr_t red=0x1007e5,blue=0x184765,target=0x283c85,mirror=0x764c85;
    auto use=std::make_shared<Use>(Use{std::make_shared<Access>(),12});
    for(auto addr:{red,blue,target,mirror}){auto lock=views.guard(0,views.mask(addr,length));
        for(unsigned i=0;i<length;++i){View v{(addr==blue?200u:100u)+i,(i%3)?use:nullptr};views.write(addr+i*32,&v);}}
    auto copy=[&](auto s,auto d){auto lock=views.guard(views.mask(s,length),views.mask(d,length));views.copy(s,d,length);};
    std::atomic<bool> bad{};auto inspect=[&](auto addr){
        auto lock=views.guard(views.mask(addr,length),0);auto base=views.find(addr)->generation;
        for(unsigned i=0;i<length;++i){auto v=views.find(addr+32*i);if(!v || v->generation!=base+i || v->upload!=((i%3)?use:nullptr))bad=true;}
    };
    {std::vector<std::jthread> ts;
        for(unsigned k=0;k<4;++k)ts.emplace_back([&,k]{for(unsigned i=0;i<20000;++i){
            if(k==0)copy(i&1?red:blue,target);else if(k==1)copy(target,mirror);else inspect(k==2?target:mirror);}});
    }
    if(bad)throw std::runtime_error("torn multi-bank publication");
    unsigned pending=0;{auto hold=views.guard(0,~0u);views.collect(red,mirror+32*length,[&](auto& u){if(u!=use)bad=true;++pending;});}
    if(bad || pending!=4*(length-(length+2)/3))throw std::runtime_error("unbounded sparse collection");
    std::puts("Concurrent descriptor copies: 40,000 coherent multi-page reads, including a concurrent copy of the destination");
    size_t scattered=0;
    for(unsigned stride:{16u,48u,64u}){
        StripedViews<View,SharedLock> v;v.increment(stride);std::array<View,384> ref;
        auto at=[&](unsigned i){return 0x100003ull+(i/128)*0x31111ull+(i%128)*stride;};
        for(unsigned i=0;i<ref.size();++i){if(i%7)ref[i]={i+1,(i&2)?use:nullptr};
            if(ref[i].generation)v.write(at(i),&ref[i]);}
        for(unsigned j=0;j<1200;++j){unsigned n=1+rng()%80,dst=(rng()%3)*128+rng()%(129-n);
            std::vector<unsigned> sources(n);for(auto& x:sources)x=rng()%ref.size();
            // Include source handles which refer to earlier destinations.
            // Comparison deliberately follows the bridge's sequential order.
            if(n>4)sources[n-1]=dst;
            v.scatter(at(dst),n,[&](unsigned k){return at(sources[k]);});
            for(unsigned k=0;k<n;++k)ref[dst+k]=ref[sources[k]];
            for(unsigned k=0;k<n;++k){auto p=v.find(at(dst+k));auto& q=ref[dst+k];
                if((p?p->generation:0)!=q.generation || (p && p->upload!=q.upload))throw std::runtime_error("incorrect scattered view");
                ++scattered;}
            if(j%7==0){auto i=rng()%ref.size();ref[i]={j+500,(rng()&1)?use:nullptr};v.write(at(i),&ref[i]);}
        }
    }
    std::printf("Ordered scatter publication: %zu descriptor and pending-image pairs matched\n",scattered);
}
