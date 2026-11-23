#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include "shader_client.hpp"
#include "play_timing.hpp"
#include "../src/view_fields.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <deque>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <wrl/client.h>

namespace {
bool stripped(D3D12_SHADER_BYTECODE b) {
    if(!b.pShaderBytecode || b.BytecodeLength<32 || b.BytecodeLength>16*1024*1024)return false;
    const auto* p=static_cast<const BYTE*>(b.pShaderBytecode);
    auto read=[&](size_t i){UINT v;std::memcpy(&v,p+i,4);return v;};
    size_t count=read(28);
    if(std::memcmp(p,"DXBC",4) || read(24)!=b.BytecodeLength || count>(b.BytecodeLength-32)/4)return false;
    for(size_t i=0;i<count;++i) {
        size_t off=read(32+4*i);
        if(off>b.BytecodeLength-8 || std::memcmp(p+off,"DXIL",4))continue;
        size_t n=read(off+4);
        if(n<24 || n>b.BytecodeLength-off-8)continue;
        size_t bc=8ull+read(off+24);
        if(bc+4<=n && read(off+28)==4 && !std::memcmp(p+off+8+bc,"NOBC",4))return true;
    }
    return false;
}
struct Connection {
    SOCKET fd=INVALID_SOCKET;
    ~Connection(){if(fd!=INVALID_SOCKET)closesocket(fd);}
    bool io(void* data,size_t len,bool sending) {
        auto* p=static_cast<char*>(data);
        while(len){int n=sending?send(fd,p,int(len),0):recv(fd,p,int(len),0);
            if(n<=0)return false;
            p+=n;len-=n;}
        return true;
    }
    bool put(const void* p,size_t n){return io(const_cast<void*>(p),n,true);}
    bool get(void* p,size_t n){return io(p,n,false);}
    bool request(const char* token,UINT (&header)[3],std::span<const BYTE> params,D3D12_SHADER_BYTECODE shader){
        WSABUF part[4]{{32,const_cast<char*>(token)},{sizeof(header),reinterpret_cast<char*>(header)},
            {ULONG(params.size()),reinterpret_cast<char*>(const_cast<BYTE*>(params.data()))},
            {ULONG(shader.BytecodeLength),static_cast<char*>(const_cast<void*>(shader.pShaderBytecode))}};
        unsigned i=0;while(i<4){
            DWORD sent{};if(WSASend(fd,part+i,4-i,&sent,0,nullptr,nullptr) || !sent)return false;
            while(i<4 && sent>=part[i].len){sent-=part[i].len;++i;}
            if(i<4){part[i].buf+=sent;part[i].len-=sent;}
        }return true;
    }
};
struct ShaderPackets{
    struct Packet{std::vector<BYTE> original,params;UINT stage;std::array<char,32> service;
        std::vector<UINT> bindings;Microsoft::WRL::ComPtr<ID3DBlob> code;UINT64 id;
        size_t size()const{return original.size()+params.size()+4*bindings.size()+code->GetBufferSize();}};
    std::mutex lock;std::deque<std::unique_ptr<Packet>> fifo;std::unordered_multimap<UINT64,Packet*> entries;size_t bytes{};
    static UINT64 hash(D3D12_SHADER_BYTECODE b,UINT stage,std::span<const BYTE> params){
        UINT64 h=0xcbf29ce484222325ull^stage;
        for(BYTE c:std::span(static_cast<const BYTE*>(b.pShaderBytecode),b.BytecodeLength))h=(h^c)*0x100000001b3ull;
        for(BYTE c:params)h=(h^c)*0x100000001b3ull;
        return h;
    }
    Packet* find(UINT64 key,D3D12_SHADER_BYTECODE b,UINT stage,std::span<const BYTE> params,const char* service){
        auto [it,end]=entries.equal_range(key);
        for(;it!=end;++it){auto p=it->second;
            if(p->stage==stage && p->original.size()==b.BytecodeLength && !std::memcmp(p->service.data(),service,32) &&
               std::ranges::equal(p->params,params) && !std::memcmp(p->original.data(),b.pShaderBytecode,b.BytecodeLength))return p;}
        return nullptr;
    }
    bool get(UINT64 k,D3D12_SHADER_BYTECODE b,UINT s,std::span<const BYTE> params,const char* token,std::vector<UINT>& views,ID3DBlob** out){
        std::lock_guard g(lock);auto p=find(k,b,s,params,token);if(!p)return false;
        views=p->bindings;p->code.CopyTo(out);return true;
    }
    void put(UINT64 k,D3D12_SHADER_BYTECODE b,UINT s,std::span<const BYTE> params,const char* token,const std::vector<UINT>& views,ID3DBlob* code)try{
        std::lock_guard g(lock);if(find(k,b,s,params,token))return;
        auto p=std::make_unique<Packet>();p->original.assign(static_cast<const BYTE*>(b.pShaderBytecode),static_cast<const BYTE*>(b.pShaderBytecode)+b.BytecodeLength);
        p->params.assign(params.begin(),params.end());p->stage=s;std::memcpy(p->service.data(),token,32);p->bindings=views;p->code=code;p->id=k;
        auto size=p->size();
        while(!fifo.empty() && bytes+size>128u<<20){
            auto old=fifo.front().get();auto [a,z]=entries.equal_range(old->id);for(;a!=z;++a)if(a->second==old){entries.erase(a);break;}
            bytes-=old->size();fifo.pop_front();
        }
        fifo.push_back(std::move(p));try{entries.emplace(k,fifo.back().get());}catch(...){fifo.pop_back();throw;}bytes+=size;
    }catch(const std::bad_alloc&){/* The persistent shader service remains authoritative. */}
};
}
HRESULT request_shader(D3D12_SHADER_BYTECODE b,UINT stage,std::span<const BYTE> types,
                       std::vector<UINT>& bindings,ID3DBlob** result) {
    *result=nullptr;
    char port[8]{},token[33]{};
    if(!GetEnvironmentVariableA("FROSTY_COMPILER_PORT",port,sizeof(port)))return S_FALSE;
    if(stage>=6 && stage<=8){
        UINT version{};if(!b.pShaderBytecode || b.BytecodeLength<32 || b.BytecodeLength>16*1024*1024 || types.size()<13 || types.size()>(stage==6?8192u:256u))return E_INVALIDARG;
        std::memcpy(&version,b.pShaderBytecode,4);if(version!=2)return E_INVALIDARG;
    }else if(!stripped(b))return S_FALSE;
    play_timing::Scope time(play_timing::requests);
    if(GetEnvironmentVariableA("FROSTY_COMPILER_TOKEN",token,sizeof(token))!=32)return E_UNEXPECTED;
    // The normal launch pins its host compiler. Hot-repair sessions can
    // change it in place and must always go back to the service.
    static const bool memo=!GetEnvironmentVariableA("FROSTY_HOLD_LIFTS",nullptr,0);
    static ShaderPackets packets;auto clean=types;
    if((stage&255)!=6 && (stage&255)!=7 && (stage&255)!=8 && std::ranges::all_of(clean,[](BYTE t){return !t;}))clean={};
    auto key=memo?ShaderPackets::hash(b,stage,clean):0;
    if(memo && packets.get(key,b,stage,clean,token,bindings,result))return S_OK;
    static int error=[](){WSADATA w{};return WSAStartup(MAKEWORD(2,2),&w);}();
    if(error)return HRESULT_FROM_WIN32(error);
    Connection c;c.fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(c.fd==INVALID_SOCKET)return HRESULT_FROM_WIN32(WSAGetLastError());
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(static_cast<u_short>(std::atoi(port)));
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(c.fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))!=0)return HRESULT_FROM_WIN32(WSAGetLastError());
    DWORD wait=GetEnvironmentVariableA("FROSTY_HOLD_LIFTS",nullptr,0)?1200000:150000;
    setsockopt(c.fd,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&wait),sizeof(wait));
    UINT req[]{stage,UINT(b.BytecodeLength),UINT(types.size())},status{};
    if(!c.request(token,req,types,b) || !c.get(&status,4))return E_FAIL;
    UINT n=0;if(!c.get(&n,4))return E_FAIL;
    if(status) {
        if(n>2048)return E_FAIL;
        std::string text(n,'\0');if(!c.get(text.data(),n))return E_FAIL;
        text="frosty/compiler: "+text+"\n";
        OutputDebugStringA(text.c_str());std::fputs(text.c_str(),stderr);return E_NOTIMPL;
    }
    UINT length=0;if(n>256 || !c.get(&length,4) || length<32 || length>16*1024*1024)return E_FAIL;
    bindings.resize(n);if(!c.get(bindings.data(),n*4))return E_FAIL;
    for(auto v:bindings)if(stage==6?(v&~(0x17fu|frosty::view::ray_as))!=0:!frosty::view::valid(v))return E_INVALIDARG;
    auto hr=D3DCreateBlob(length,result);if(FAILED(hr))return hr;
    if(!c.get((*result)->GetBufferPointer(),length) || std::memcmp((*result)->GetBufferPointer(),"DXBC",4)) {
        (*result)->Release();*result=nullptr;return E_FAIL;
    }
    if(memo)packets.put(key,b,stage,clean,token,bindings,*result);
    return S_OK;
}
HRESULT request_ray_leaf(std::span<const BYTE> xsrt,UINT stage,UINT words,UINT word,UINT value,std::string_view name,ID3DBlob** out){
    if((stage!=7 && stage!=8) || name.empty() || name.size()>244)return E_INVALIDARG;
    UINT fields[]{words,word,value};std::vector<BYTE> spec(12+name.size());
    std::memcpy(spec.data(),fields,sizeof(fields));std::memcpy(spec.data()+12,name.data(),name.size());
    std::vector<UINT> views;return request_shader({xsrt.data(),xsrt.size()},stage,spec,views,out);
}
HRESULT request_raygen(std::span<const BYTE> xsrt,UINT words,UINT route,UINT word,UINT value,std::string_view layout,std::vector<UINT>& bindings,ID3DBlob** out){
    UINT fields[]{words,route,word,value};if(layout.empty() || layout.size()>8192-sizeof(fields))return E_INVALIDARG;
    std::vector<BYTE> spec(sizeof(fields)+layout.size());std::memcpy(spec.data(),fields,sizeof(fields));std::memcpy(spec.data()+sizeof(fields),layout.data(),layout.size());
    return request_shader({xsrt.data(),xsrt.size()},6,spec,bindings,out);
}
