#define NOMINMAX
#include "tile_plan.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>

namespace {
using Hash=std::array<BYTE,32>;
constexpr size_t max_file=32u<<20,mem_budget=128u<<20,disk_budget=512u<<20;
constexpr UINT tag=0x31505458; // XTP1
static_assert(sizeof(TileRun)==8);
bool digest(const void* data,size_t size,Hash& out){
    static auto alg=[](){BCRYPT_ALG_HANDLE h{};return BCryptOpenAlgorithmProvider(&h,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0?h:nullptr;}();
    return alg && size<=UINT_MAX && BCryptHash(alg,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<void*>(data)),ULONG(size),out.data(),out.size())>=0;
}
struct Header {TileKey key;UINT magic,rows,runs,pad;Hash hash;};
using File=std::unique_ptr<FILE,decltype(&std::fclose)>;
struct Cache{
    std::wstring dir;
    std::mutex lock;std::map<TileKey,std::shared_ptr<const TilePlan>> ready;std::deque<TileKey> fifo;
    std::array<std::mutex,64> builders;size_t bytes{};std::atomic<size_t> disk{};
    Cache(){
        wchar_t path[2048]{};auto n=GetEnvironmentVariableW(L"FROSTY_TILE_PLANS",path,2048);if(!n || n>=2048)return;
        std::error_code ec;std::filesystem::path p(path);if(!std::filesystem::is_directory(p,ec) || ec)return;
        for(std::filesystem::directory_iterator it(p,ec),end;!ec && it!=end;it.increment(ec))
            if(it->path().extension()==L".xtp"){auto size=it->file_size(ec);if(!ec)disk+=size;}
        if(!ec)dir=path;
    }
    std::shared_ptr<const TilePlan> find(const TileKey& k){
        std::lock_guard g(lock);auto it=ready.find(k);return it!=ready.end()?it->second:nullptr;
    }
    std::wstring name(const Hash& h)const{
        if(dir.empty())return {};
        auto p=dir+L"\\";constexpr wchar_t hex[]=L"0123456789abcdef";
        for(auto b:h){p+=hex[b>>4];p+=hex[b&15];}return p+L".xtp";
    }
    static std::shared_ptr<TilePlan> load(const std::wstring& path,const TileKey& k,UINT64 limit,UINT width,UINT rows){
        if(path.empty())return {};
        File f(_wfopen(path.c_str(),L"rb"),std::fclose);if(!f)return {};
        Header h{};if(std::fread(&h,1,sizeof(h),f.get())!=sizeof(h) || h.key!=k || h.magic!=tag || h.rows!=rows || h.pad)return {};
        UINT64 n=UINT64(h.rows)*4+UINT64(h.runs)*8;if(!h.runs || n>max_file)return {};
        std::vector<BYTE> b(n);Hash sha{};
        if(std::fread(b.data(),1,b.size(),f.get())!=b.size() || std::fgetc(f.get())!=EOF || !digest(b.data(),b.size(),sha) || sha!=h.hash)return {};
        auto plan=std::make_shared<TilePlan>();plan->ends.resize(rows);plan->runs.resize(h.runs);
        std::memcpy(plan->ends.data(),b.data(),4ull*rows);std::memcpy(plan->runs.data(),b.data()+4ull*rows,8ull*h.runs);
        size_t last=0;for(UINT end:plan->ends){
            if(end<=last || end>plan->runs.size())return {};
            UINT64 sum=0;for(;last<end;++last){auto [src,len]=plan->runs[last];auto hi=UINT64(src)+len;
                if(!len || hi>limit || hi>UINT_MAX)return {};
                sum+=len;plan->lo=std::min(plan->lo,src);plan->hi=std::max(plan->hi,UINT(hi));}
            if(sum!=width)return {};
        }
        return last==plan->runs.size()?plan:nullptr;
    }
    void save(const std::wstring& path,const TileKey& k,const TilePlan& plan){
        if(path.empty() || plan.size()>max_file || disk.load()+plan.size()>disk_budget)return;
        Header h{k,tag,UINT(plan.ends.size()),UINT(plan.runs.size()),0,{}};
        std::vector<BYTE> data(plan.size());auto bytes=plan.ends.size()*4;
        std::memcpy(data.data(),plan.ends.data(),bytes);std::memcpy(data.data()+bytes,plan.runs.data(),plan.runs.size()*8);
        if(!digest(data.data(),data.size(),h.hash))return;
        auto tmp=path+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId())+L".tmp";
        File f(_wfopen(tmp.c_str(),L"wb"),std::fclose);if(!f)return;
        bool ok=std::fwrite(&h,1,sizeof(h),f.get())==sizeof(h) && std::fwrite(data.data(),1,data.size(),f.get())==data.size() && !std::fflush(f.get());
        f.reset();if(ok && MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))disk+=data.size()+sizeof(h);
        else DeleteFileW(tmp.c_str());
    }
};
}
bool tile_sha(const void* data,size_t n,std::array<BYTE,32>& h){return digest(data,n,h);}
std::wstring tile_path(const TileKey& key,const wchar_t* extension){
    wchar_t p[2048]{};auto n=GetEnvironmentVariableW(L"FROSTY_TILE_PLANS",p,2048);Hash h{};
    if(!n || n>=2048 || !digest(key.data(),sizeof(key),h))return {};
    std::wstring s=p;s+=L"\\";constexpr wchar_t hex[]=L"0123456789abcdef";
    for(auto b:h){s+=hex[b>>4];s+=hex[b&15];}return s+extension;
}
std::shared_ptr<const TilePlan> exact_tile_plan(const TileKey& k,UINT64 limit,UINT width,UINT rows,
        const std::function<std::shared_ptr<TilePlan>()>& build)try{
    static Cache cache;if(auto p=cache.find(k))return p;
    Hash key{};if(!digest(k.data(),sizeof(k),key))return build();
    std::lock_guard guard(cache.builders[key[0]%cache.builders.size()]);
    if(auto p=cache.find(k))return p;
    auto path=cache.name(key);auto p=cache.load(path,k,limit,width,rows);
    if(!p){p=build();if(!p)return {};cache.save(path,k,*p);}
    {std::lock_guard g(cache.lock);
        while(!cache.fifo.empty() && (cache.bytes+p->size()>mem_budget || cache.fifo.size()>=4096)){
            auto i=cache.ready.find(cache.fifo.front());cache.bytes-=i->second->size();cache.ready.erase(i);cache.fifo.pop_front();}
        cache.fifo.push_back(k);try{cache.ready.emplace(k,p);}catch(...){cache.fifo.pop_back();throw;}cache.bytes+=p->size();}
    return p;
}catch(const std::bad_alloc&){return {};} // The direct XG lookup path remains available.
