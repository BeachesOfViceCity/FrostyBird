#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

// Optional read-only acceleration for immutable Zlib I/O Store blocks.
// Location and extent only select a candidate. Verify the actual compressed
// input on every hit and the mapped output before its first publication.
class StoragePack {
    using Hash=std::array<BYTE,32>;
    struct Header{char magic[8];UINT count,stride;Hash index;};
    struct Row{UINT64 offset;UINT compressed,decoded;UINT64 payload;Hash input,output;};
    static_assert(sizeof(Header)==48 && sizeof(Row)==88);
    HANDLE file_=INVALID_HANDLE_VALUE,mapping_{};const BYTE* data_{};const Row* rows_{};
    std::unique_ptr<std::atomic<BYTE>[]> verified_;UINT count_{};
    BCRYPT_ALG_HANDLE sha_{};
    std::atomic<UINT64> hits_{},bytes_{},rejected_{};
    bool match(const BYTE* src,UINT n,const Hash& expected)const{
        Hash actual;return BCryptHash(sha_,nullptr,0,const_cast<BYTE*>(src),n,actual.data(),UINT(actual.size()))>=0 && actual==expected;
    }
public:
    StoragePack(){
        wchar_t path[2048];auto n=GetEnvironmentVariableW(L"FROSTY_STORAGE_PACK",path,2048);if(!n || n>=2048)return;
        file_=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        LARGE_INTEGER size{};
        if(file_==INVALID_HANDLE_VALUE || !GetFileSizeEx(file_,&size) || size.QuadPart<LONGLONG(sizeof(Header)) || size.QuadPart>(64ll<<30))return;
        mapping_=CreateFileMappingW(file_,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping_)return;
        data_=static_cast<const BYTE*>(MapViewOfFile(mapping_,FILE_MAP_READ,0,0,0));if(!data_)return;
        auto* h=reinterpret_cast<const Header*>(data_);auto length=UINT64(size.QuadPart);
        if(std::memcmp(h->magic,"XSDPACK1",8) || h->stride!=sizeof(Row) || !h->count || h->count>1000000 ||
           sizeof(Header)+h->count*sizeof(Row)>length || BCryptOpenAlgorithmProvider(&sha_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return;
        auto* r=reinterpret_cast<const Row*>(data_+sizeof(Header));
        if(!match(reinterpret_cast<const BYTE*>(r),UINT(h->count*sizeof(Row)),h->index))return;
        UINT64 next=sizeof(Header)+h->count*sizeof(Row);
        for(UINT i=0;i<h->count;++i){
            if((i && r[i-1].offset>=r[i].offset) || !r[i].compressed || !r[i].decoded || r[i].decoded>(2u<<20) ||
               r[i].payload!=next || r[i].decoded>length-next)return;
            next+=r[i].decoded;
        }
        if(next!=length)return;
        verified_.reset(new(std::nothrow) std::atomic<BYTE>[h->count]{});if(!verified_)return;
        rows_=r;count_=h->count;
    }
    bool copy(UINT64 offset,const BYTE* input,UINT packed,void* dst,UINT size){
        if(!rows_)return false;
        auto r=std::lower_bound(rows_,rows_+count_,offset,[](const Row& a,UINT64 b){return a.offset<b;});
        if(r==rows_+count_ || r->offset!=offset || r->compressed!=packed || r->decoded!=size)return false;
        auto& valid=verified_[r-rows_];auto state=valid.load(std::memory_order_relaxed);
        if(state==2 || !match(input,packed,r->input)){rejected_.fetch_add(1,std::memory_order_relaxed);return false;}
        auto source=data_+r->payload;
        if(!state){bool good=match(source,size,r->output);valid.store(good?1:2,std::memory_order_relaxed);
            if(!good){rejected_.fetch_add(1,std::memory_order_relaxed);return false;}}
        std::memcpy(dst,source,size);hits_.fetch_add(1,std::memory_order_relaxed);bytes_.fetch_add(size,std::memory_order_relaxed);return true;
    }
    UINT64 hits()const{return hits_.load(std::memory_order_relaxed);}
    UINT64 bytes()const{return bytes_.load(std::memory_order_relaxed);}
    UINT64 rejected()const{return rejected_.load(std::memory_order_relaxed);}
    ~StoragePack(){
        if(sha_)BCryptCloseAlgorithmProvider(sha_,0);
        if(data_)UnmapViewOfFile(data_);
        if(mapping_)CloseHandle(mapping_);
        if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
    }
};
