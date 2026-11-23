#define NOMINMAX
#include "save_store.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// One atomically replaced file per GDK container. Render and game threads may
// open independent handles. An empty account returns no blobs, not a fixture.
namespace local_save {
namespace fs=std::filesystem;
namespace {
using Bytes=std::vector<BYTE>;
using Data=std::map<std::string,Bytes>;
constexpr size_t limit=64u<<20;
constexpr HRESULT not_found=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
struct Provider {fs::path dir;};
struct Container {fs::path file;std::string name;};
struct Update {Container c;std::map<std::string,std::optional<Bytes>> changes;};
std::mutex lock;
std::string encode(const char* text) {
    const std::string s=text?text:"";if(s.empty() || s.size()>112)throw E_INVALIDARG;
    constexpr char hex[]="0123456789abcdef";std::string out;
    for(unsigned char c:s){out+=hex[c>>4];out+=hex[c&15];}
    return out;
}
std::string decode(const std::string& s){
    auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    std::string out;if(s.size()%2)return out;
    for(size_t i=0;i<s.size();i+=2){auto a=digit(s[i]),b=digit(s[i+1]);if(a<0||b<0)return {};out+=char(a*16+b);}return out;
}
Data load(const fs::path& file) {
    Data result;if(!fs::exists(file))return result;
    if(fs::file_size(file)>limit)throw E_FAIL;
    std::ifstream in(file,std::ios::binary);in.exceptions(std::ios::badbit|std::ios::failbit);
    UINT magic{},count{};in.read(reinterpret_cast<char*>(&magic),4);in.read(reinterpret_cast<char*>(&count),4);
    if(magic!=0x31534746 || count>65536)throw E_FAIL;
    for(UINT i=0;i<count;++i){
        UINT n{},len{};in.read(reinterpret_cast<char*>(&n),4);in.read(reinterpret_cast<char*>(&len),4);
        if(!n || n>255 || len>limit)throw E_FAIL;
        std::string name(n,0);Bytes data(len);in.read(name.data(),n);if(len)in.read(reinterpret_cast<char*>(data.data()),len);
        if(!result.emplace(std::move(name),std::move(data)).second)throw E_FAIL;
    }
    return result;
}
HRESULT notify(const Container& c,void* ctx,ContainerCallback fn){
    auto data=load(c.file);UINT64 size=0;for(auto& [_,bytes]:data)size+=bytes.size();
    WIN32_FILE_ATTRIBUTE_DATA a{};if(!GetFileAttributesExW(c.file.c_str(),GetFileExInfoStandard,&a))return HRESULT_FROM_WIN32(GetLastError());
    ULARGE_INTEGER ft{};ft.HighPart=a.ftLastWriteTime.dwHighDateTime;ft.LowPart=a.ftLastWriteTime.dwLowDateTime;
    ContainerInfo i{c.name.c_str(),c.name.c_str(),UINT(data.size()),size,INT64(ft.QuadPart/10000000)-11644473600ll,FALSE};
    return fn(&i,ctx)?S_OK:S_FALSE;
}
}
#define SAVE_ERRORS catch(HRESULT hr){return hr;} catch(const std::bad_alloc&){return E_OUTOFMEMORY;} catch(const std::exception&){return E_FAIL;}
HRESULT initialize(UINT64 user,const char* scid,void** out) try {
    if(!out)return E_POINTER;
    *out=nullptr;char id[24];std::snprintf(id,sizeof(id),"%016llx",user);
    auto dir=fs::path(L"T:\\Settings\\Frosty\\LocalSaves")/id/encode(scid);
    fs::create_directories(dir);*out=new Provider{std::move(dir)};return S_OK;
} SAVE_ERRORS
void WINAPI close_provider(void*,void* h){delete static_cast<Provider*>(h);}
HRESULT WINAPI create(void*,void* p,const char* name,void** out) try {
    if(!p || !out)return E_POINTER;
    *out=nullptr;*out=new Container{static_cast<Provider*>(p)->dir/(encode(name)+".gs"),name};return S_OK;
} SAVE_ERRORS
void WINAPI close(void*,void* c){delete static_cast<Container*>(c);}
HRESULT WINAPI remove(void*,void* p,const char* name) try {
    if(!p)return E_HANDLE;
    std::lock_guard guard(lock);
    return fs::remove(static_cast<Provider*>(p)->dir/(encode(name)+".gs"))?S_OK:not_found;
} SAVE_ERRORS
HRESULT WINAPI info(void*,void* p,const char* name,void* ctx,ContainerCallback fn) try {
    if(!p || !fn)return E_INVALIDARG;
    Container c{static_cast<Provider*>(p)->dir/(encode(name)+".gs"),name};
    if(!fs::exists(c.file))return S_OK;
    auto hr=notify(c,ctx,fn);return FAILED(hr)?hr:S_OK;
} SAVE_ERRORS
HRESULT WINAPI enumerate_by(void*,void* p,const char* prefix,void* ctx,ContainerCallback fn) try {
    if(!p || !fn)return E_INVALIDARG;
    for(auto& e:fs::directory_iterator(static_cast<Provider*>(p)->dir))if(e.path().extension()==".gs"){
        auto name=decode(e.path().stem().string());if(name.empty() || (prefix && !name.starts_with(prefix)))continue;
        auto hr=notify({e.path(),name},ctx,fn);if(FAILED(hr))return hr;if(hr==S_FALSE)break;
    }
    return S_OK;
} SAVE_ERRORS
HRESULT WINAPI enumerate(void* a,void* p,void* ctx,ContainerCallback fn){return enumerate_by(a,p,nullptr,ctx,fn);}
HRESULT WINAPI blobs_by(void*,void* h,const char* prefix,void* ctx,BlobCallback fn) try {
    if(!h || !fn)return E_INVALIDARG;
    auto data=load(static_cast<Container*>(h)->file);
    for(auto& [name,bytes]:data){if(prefix && !name.starts_with(prefix))continue;
        BlobInfo info{name.c_str(),UINT(bytes.size())};if(!fn(&info,ctx))break;}
    return S_OK;
} SAVE_ERRORS
HRESULT WINAPI blobs(void* a,void* c,void* ctx,BlobCallback fn){return blobs_by(a,c,nullptr,ctx,fn);}
HRESULT WINAPI read(void*,void* h,const char** names,UINT* count,SIZE_T cap,Blob* out) try {
    if(!h || !count || !out)return E_INVALIDARG;
    auto data=load(static_cast<Container*>(h)->file);std::vector<Data::const_iterator> chosen;
    if(names){for(UINT i=0;i<*count;++i){if(!names[i])return E_INVALIDARG;auto it=data.find(names[i]);if(it==data.end())return not_found;chosen.push_back(it);}}
    else for(auto it=data.cbegin();it!=data.end();++it)chosen.push_back(it);
    size_t need=chosen.size()*sizeof(Blob);
    for(auto it:chosen)need+=it->second.size()+it->first.size()+1;
    if(need>cap)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    BYTE* dst=reinterpret_cast<BYTE*>(out+chosen.size());*count=UINT(chosen.size());
    for(auto it:chosen){auto& [name,bytes]=*it;size_t n=name.size()+1;
        out->info={reinterpret_cast<const char*>(dst),UINT(bytes.size())};std::memcpy(dst,name.c_str(),n);dst+=n;
        out->data=dst;if(!bytes.empty())std::memcpy(dst,bytes.data(),bytes.size());dst+=bytes.size();++out;}
    return S_OK;
} SAVE_ERRORS
HRESULT WINAPI update(void*,void* h,const char*,void** out) try {
    if(!h || !out)return E_INVALIDARG;
    *out=new Update{*static_cast<Container*>(h),{}};return S_OK;
} SAVE_ERRORS
void WINAPI close_update(void*,void* h){delete static_cast<Update*>(h);}
HRESULT WINAPI write(void*,void* h,const char* name,const BYTE* data,SIZE_T n) try {
    if(!h || !name || !*name || std::strlen(name)>255 || n>limit || (!data&&n))return E_INVALIDARG;
    static_cast<Update*>(h)->changes[name]=n?Bytes(data,data+n):Bytes{};return S_OK;
} SAVE_ERRORS
HRESULT WINAPI erase(void*,void* h,const char* name) try {
    if(!h||!name)return E_INVALIDARG;
    static_cast<Update*>(h)->changes[name]=std::nullopt;return S_OK;
} SAVE_ERRORS
HRESULT WINAPI submit(void*,void* h) try {
    if(!h)return E_HANDLE;
    auto& u=*static_cast<Update*>(h);std::lock_guard guard(lock);
    auto data=load(u.c.file);
    for(auto& [n,b]:u.changes){if(b)data[n]=*b;else data.erase(n);}
    size_t total=8;for(auto& [n,b]:data)total+=8+n.size()+b.size();if(total>limit)return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    auto tmp=u.c.file;tmp+=L".tmp";
    std::ofstream f;f.exceptions(std::ios::badbit|std::ios::failbit);f.open(tmp,std::ios::binary|std::ios::trunc);
    auto word=[&](UINT n){f.write(reinterpret_cast<const char*>(&n),4);};
    word(0x31534746);word(UINT(data.size()));
    for(auto& [n,b]:data){word(UINT(n.size()));word(UINT(b.size()));f.write(n.data(),n.size());if(!b.empty())f.write(reinterpret_cast<const char*>(b.data()),b.size());}
    f.close();if(!MoveFileExW(tmp.c_str(),u.c.file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        return HRESULT_FROM_WIN32(GetLastError());
    u.changes.clear();return S_OK;
} SAVE_ERRORS
}
