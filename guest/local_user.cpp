// Offline, per-userdata GDK session. The compatibility runtime still owns
// XAsync and the queues; there is no Microsoft account or online credential.
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>
#include <string>
#include "save_store.hpp"
#include "controller_seat.hpp"

namespace {
using Slot=void(*)();
template<class F> Slot slot(F f){return std::bit_cast<Slot>(f);}
struct Api {const Slot* table;};
template<class Fn> Fn method(Api* a,unsigned i){return std::bit_cast<Fn>(a->table[i]);}
constexpr GUID user_id{0x01acd177,0x91f9,0x4763,{0xa3,0x8e,0xcc,0xbb,0x55,0xce,0x32,0xe0}};
constexpr GUID user2_id{0xeb9bf948,0x18dc,0x4d82,{0xbb,0xcc,0x40,0xe0,0xa8,0x09,0xc4,0xc0}};
constexpr GUID tag_id{0xcef4fac0,0x7676,0x4a94,{0xa1,0x19,0x4c,0x43,0xf9,0xeb,0x5b,0x74}};
constexpr GUID device_id{0x7d824997,0x10dc,0x45ab,{0x86,0xb7,0x27,0x37,0x76,0x7c,0x0b,0xf1}};
constexpr GUID device2_id{0x0cc6a956,0xe7e1,0x4fdf,{0x93,0x41,0x9d,0x5d,0xa6,0x49,0xeb,0xc8}};
constexpr GUID threading_id{0x073b7dcb,0x1fcf,0x4030,{0x94,0xbe,0xe3,0xc9,0xeb,0x62,0x34,0x28}};
constexpr GUID network_id{0x37e56907,0x2f10,0x41e8,{0xb7,0x2f,0x36,0xed,0xb1,0x85,0x33,0x1a}};
constexpr GUID network2_id{0xbf2346b2,0x39af,0x4658,{0xb5,0xea,0x44,0x71,0x3c,0x7e,0x83,0xb3}};
constexpr GUID save_id{0x704c3f58,0xe629,0x4cc2,{0xb1,0x97,0x30,0x51,0x1b,0x99,0x6f,0xe2}};
constexpr GUID feature_id{0x8836fe87,0xedb9,0x4fe3,{0x8d,0xad,0x05,0xf0,0xd2,0xcd,0x5b,0x40}};
using Query=HRESULT(WINAPI*)(REFGUID,REFIID,void**);
Query core() {
    static auto query=[]{
        auto h=LoadLibraryExW(L"frosty_grt_core.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        return h?std::bit_cast<Query>(GetProcAddress(h,"QueryApiImpl")):nullptr;
    }();
    return query;
}
struct Profile {
    UINT64 id{};std::array<char,16> name{};
    Profile() {
        char text[32]{};
        GetEnvironmentVariableA("FROSTY_LOCAL_ID",text,sizeof(text));
        id=std::strtoull(text,nullptr,16);
        auto n=GetEnvironmentVariableA("FROSTY_LOCAL_PLAYER",name.data(),UINT(name.size()));
        if(!n || n>=name.size())std::strcpy(name.data(),"Player");
    }
};
Profile& profile(){static Profile p;return p;}
bool valid(void* p){return p==&profile() && profile().id;}
constexpr auto offline=HRESULT_FROM_WIN32(ERROR_NOT_CONNECTED);
HRESULT WINAPI query_iface(Api*,REFIID,void**);
ULONG WINAPI ref(Api*){return 2;}
HRESULT WINAPI unavailable(Api*,...){return offline;}
HRESULT WINAPI duplicate(Api*,void* h,void** out){if(!out)return E_POINTER;*out=valid(h)?h:nullptr;return *out?S_OK:E_HANDLE;}
void WINAPI close_user(Api*,void*){} // handle denotes the process-lifetime offline seat
INT WINAPI compare(Api*,void* a,void* b){return a==b?0:a<b?-1:1;}
HRESULT WINAPI max_users(Api*,UINT* n){if(!n)return E_POINTER;*n=1;return S_OK;}
Api* threading(){
    static auto* a=[]()->Api*{Api* value{};auto q=core();if(q)q(threading_id,threading_id,reinterpret_cast<void**>(&value));return value;}();
    return a;
}
struct ProviderData {void* async;SIZE_T size;void* buffer;void* context;};
HRESULT WINAPI provide(unsigned op,const ProviderData* d){
    if(op==2){if(!d || !d->buffer || d->size<sizeof(void*))return E_INVALIDARG;
        auto h=&profile();std::memcpy(d->buffer,&h,sizeof(h));}
    return S_OK;
}
const char add_identity[]="FrostyLocalUserAdd";
HRESULT WINAPI add_user(Api*,UINT opts,void* block){
    if(!block || opts&~7u || !profile().id)return E_INVALIDARG;
    auto a=threading();if(!a)return E_NOINTERFACE;
    auto hr=method<HRESULT(WINAPI*)(Api*,void*,void*,const void*,const char*,decltype(&provide))>(a,7)
        (a,block,nullptr,add_identity,add_identity,provide);
    if(SUCCEEDED(hr))method<void(WINAPI*)(Api*,void*,HRESULT,SIZE_T)>(a,10)(a,block,S_OK,sizeof(void*));
    return hr;
}
HRESULT WINAPI add_result(Api*,void* block,void** h){
    if(!block||!h)return E_POINTER;
    *h=nullptr;auto a=threading();if(!a)return E_NOINTERFACE;
    return method<HRESULT(WINAPI*)(Api*,void*,const void*,SIZE_T,void*,SIZE_T*)>(a,11)(a,block,add_identity,sizeof(*h),h,nullptr);
}
HRESULT WINAPI local(Api*,void* h,UINT64* id){if(!id)return E_POINTER;*id=valid(h)?1:0;return *id?S_OK:E_HANDLE;}
HRESULT WINAPI find_local(Api*,UINT64 id,void** h){if(!h)return E_POINTER;*h=id==1?&profile():nullptr;return *h?S_OK:E_HANDLE;}
HRESULT WINAPI get_id(Api*,void* h,UINT64* id){if(!id)return E_POINTER;*id=valid(h)?profile().id:0;return *id?S_OK:E_HANDLE;}
HRESULT WINAPI find_user(Api*,UINT64 id,void** h){return find_local(nullptr,id==profile().id?1:0,h);}
HRESULT WINAPI is_guest(Api*,void* h,BOOLEAN* b){if(!b)return E_POINTER;*b=FALSE;return valid(h)?S_OK:E_HANDLE;}
HRESULT WINAPI state(Api*,void* h,UINT* s){if(!s)return E_POINTER;*s=valid(h)?0:2;return valid(h)?S_OK:E_HANDLE;}
HRESULT WINAPI age(Api*,void* h,UINT* s){if(!s)return E_POINTER;*s=0;return valid(h)?S_OK:E_HANDLE;}
HRESULT WINAPI privilege(Api*,void* h,UINT,UINT,BOOLEAN* yes,UINT* reason) {
    if(!yes)return E_POINTER;
    *yes=FALSE;if(reason)*reason=0xffffffffu;return valid(h)?S_OK:E_HANDLE;
}
HRESULT WINAPI gamertag(Api*,void* h,UINT component,SIZE_T cap,char* out,SIZE_T* used){
    if(!valid(h))return E_HANDLE;
    if(component>3 || !out)return E_INVALIDARG;
    auto text=component==2?"":profile().name.data();auto n=std::strlen(text)+1;
    if(used)*used=n;
    if(cap<n)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(out,text,n);return S_OK;
}
std::mutex event_lock;
std::set<UINT64> tokens;
HRESULT WINAPI watch(Api*,void*,void*,void* callback,UINT64* token){
    if(!callback || !token)return E_INVALIDARG;
    static UINT64 next{};std::lock_guard lock(event_lock);tokens.insert(*token=++next);return S_OK;
}
BOOLEAN WINAPI unwatch(Api*,UINT64 t,BOOLEAN){std::lock_guard lock(event_lock);return tokens.erase(t)!=0;}
HRESULT WINAPI add_by_id(Api* a,UINT64 id,void* block){return id==profile().id?add_user(a,0,block):E_HANDLE;}
HRESULT WINAPI find_device(Api*,const APP_LOCAL_DEVICE_ID* device,void** h){
    if(!h)return E_POINTER;
    *h=nullptr;
    if(device)for(unsigned i=0;i<XUSER_MAX_COUNT;++i){
        auto seat=controller_seat_id(i);if(!std::memcmp(device,&seat,sizeof(seat)))return find_local(nullptr,1,h);
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
struct Association {APP_LOCAL_DEVICE_ID device;UINT64 old_id,new_id;};
using PairCallback=void(WINAPI*)(void*,const Association*);
struct PairEvent {PairCallback cb;void* ctx;UINT64 token;};
void WINAPI send_pair(void* ctx,BOOLEAN canceled){
    auto* e=static_cast<PairEvent*>(ctx);
    if(!canceled){
        unsigned present=0;XINPUT_STATE state{};
        for(unsigned i=0;i<XUSER_MAX_COUNT;++i)if(!XInputGetState(i,&state))present|=1u<<i;
        if(!present)present=1; // keyboard navigation owns seat zero without a physical pad
        for(unsigned i=0;i<XUSER_MAX_COUNT;++i)if(present&(1u<<i)){
            {std::lock_guard guard(event_lock);if(!tokens.contains(e->token))break;}
            Association a{controller_seat_id(i),0,1};e->cb(e->ctx,&a);
        }
    }
    delete e;
}
HRESULT WINAPI watch_pair(Api* self,void* queue,void* ctx,PairCallback cb,UINT64* token){
    auto hr=watch(self,queue,ctx,std::bit_cast<void*>(cb),token);if(FAILED(hr))return hr;
    auto* event=new(std::nothrow) PairEvent{cb,ctx,*token};if(!event)return E_OUTOFMEMORY;
    auto a=threading();
    if(!a){delete event;return E_NOINTERFACE;}
    hr=method<HRESULT(WINAPI*)(Api*,void*,UINT,void*,decltype(&send_pair))>(a,18)(a,queue,1,event,send_pair);
    if(FAILED(hr)){delete event;unwatch(self,*token,FALSE);}
    return hr;
}
template<size_t N> auto base_table(){
    std::array<Slot,N> t;t.fill(slot(&unavailable));t[0]=slot(&query_iface);t[1]=slot(&ref);t[2]=slot(&ref);return t;
}
const auto users=[]{
    auto t=base_table<37>();t[3]=slot(&duplicate);t[4]=slot(&close_user);t[5]=slot(&compare);t[6]=slot(&max_users);
    t[7]=slot(&add_user);t[8]=slot(&add_result);t[9]=slot(&local);t[10]=slot(&find_local);t[11]=slot(&get_id);
    t[12]=slot(&find_user);t[13]=slot(&is_guest);t[14]=slot(&state);t[19]=slot(&age);t[20]=slot(&privilege);
    t[33]=slot(&watch);t[34]=slot(&unwatch);
    return t;
}();
// IXUserImpl ends with a sign-out deferral and its close; IXUserImpl2 appends two methods.
const auto user2=[] {auto t=base_table<39>();std::copy(users.begin(),users.end(),t.begin());
    t[37]=slot(&add_by_id);t[38]=slot(&add_result);return t;}();
const auto tags=[]{auto t=base_table<4>();t[3]=slot(&gamertag);return t;}();
const auto devices=[]{auto t=base_table<12>();t[3]=slot(&find_device);t[4]=slot(&watch_pair);t[5]=slot(&unwatch);return t;}();
// WinHTTP handles public Internet HTTPS requests using the system trust
// store. No platform auth tokens, custom roots or certificate exceptions.
struct SecurityInfo {UINT protocols;SIZE_T count;void* pins;};
struct ConnectHint {UINT level,cost,iana;BOOLEAN initialized,near_limit,over_limit,roaming;};
const char policy_identity[]="FrostyWinHttpPolicy";
HRESULT WINAPI security_data(unsigned op,const ProviderData* d) {
    if(op==2){
        if(!d || !d->buffer || d->size<sizeof(SecurityInfo))return E_INVALIDARG;
        *static_cast<SecurityInfo*>(d->buffer)={WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2,0,nullptr};
    }
    return S_OK;
}
HRESULT WINAPI security_async(Api*,const wchar_t* url,void* block) {
    if(!url || !block)return E_INVALIDARG;
    URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=DWORD(-1);
    if(!WinHttpCrackUrl(url,0,0,&u) || (u.nScheme!=INTERNET_SCHEME_HTTPS && u.nScheme!=INTERNET_SCHEME_HTTP))return E_INVALIDARG;
    auto a=threading();if(!a)return E_NOINTERFACE;
    auto hr=method<HRESULT(WINAPI*)(Api*,void*,void*,const void*,const char*,decltype(&security_data))>(a,7)
        (a,block,nullptr,policy_identity,policy_identity,security_data);
    if(SUCCEEDED(hr))method<void(WINAPI*)(Api*,void*,HRESULT,SIZE_T)>(a,10)(a,block,S_OK,sizeof(SecurityInfo));
    return hr;
}
HRESULT WINAPI security_size(Api*,void* block,SIZE_T* size) {
    if(!block || !size)return E_INVALIDARG;
    auto a=threading();
    return a?method<HRESULT(WINAPI*)(Api*,void*,SIZE_T*)>(a,4)(a,block,size):E_NOINTERFACE;
}
HRESULT WINAPI security_result(Api*,void* block,SIZE_T cap,SIZE_T* used,BYTE* buf,SecurityInfo** out) {
    if(!block || !buf || !out)return E_INVALIDARG;
    *out=nullptr;
    auto a=threading();if(!a)return E_NOINTERFACE;
    auto hr=method<HRESULT(WINAPI*)(Api*,void*,const void*,SIZE_T,void*,SIZE_T*)>(a,11)(a,block,policy_identity,cap,buf,used);
    if(SUCCEEDED(hr))*out=reinterpret_cast<SecurityInfo*>(buf);
    return hr;
}
HRESULT WINAPI verify_server(Api*,HINTERNET request,const SecurityInfo* info) {
    if(!request || !info || info->count)return E_INVALIDARG;
    DWORD n=0;
    WinHttpQueryOption(request,WINHTTP_OPTION_URL,nullptr,&n);
    if(!n || n>65536)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::vector<wchar_t> url(n/2+1);
    if(!WinHttpQueryOption(request,WINHTTP_OPTION_URL,url.data(),&n))return HRESULT_FROM_WIN32(GetLastError());
    URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=DWORD(-1);
    if(!WinHttpCrackUrl(url.data(),0,0,&u) || u.nScheme!=INTERNET_SCHEME_HTTPS)return E_INVALIDARG;
    std::wstring host(u.lpszHostName,u.dwHostNameLength);
    PCCERT_CONTEXT cert{};n=sizeof(cert);
    if(!WinHttpQueryOption(request,WINHTTP_OPTION_SERVER_CERT_CONTEXT,&cert,&n))return HRESULT_FROM_WIN32(GetLastError());
    CERT_CHAIN_PARA cp{};cp.cbSize=sizeof(cp);
    char oid[]=szOID_PKIX_KP_SERVER_AUTH;LPSTR oids[]{oid};
    cp.RequestedUsage.Usage={1,oids};PCCERT_CHAIN_CONTEXT chain{};
    HRESULT hr=E_FAIL;
    if(CertGetCertificateChain(nullptr,cert,nullptr,cert->hCertStore,&cp,0,nullptr,&chain)){
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};ssl.cbSize=sizeof(ssl);ssl.dwAuthType=AUTHTYPE_SERVER;ssl.pwszServerName=host.data();
        CERT_CHAIN_POLICY_PARA policy{};policy.cbSize=sizeof(policy);policy.pvExtraPolicyPara=&ssl;
        CERT_CHAIN_POLICY_STATUS status{};status.cbSize=sizeof(status);
        if(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,chain,&policy,&status))hr=static_cast<HRESULT>(status.dwError);
        CertFreeCertificateChain(chain);
    }
    CertFreeCertificateContext(cert);return hr;
}
HRESULT WINAPI connectivity(Api*,ConnectHint* out) {
    if(!out)return E_POINTER;
    // Connectivity is initialized. Reachability of an Internet service is
    // established by the actual request, not by a console-service login.
    *out={0,0,0,TRUE,FALSE,FALSE,FALSE};return S_OK;
}
const auto networks=[]{auto t=base_table<21>();t[11]=slot(&security_async);t[12]=slot(&security_size);
    t[13]=slot(&security_result);t[14]=slot(&verify_server);t[15]=slot(&connectivity);t[16]=slot(&watch);t[17]=slot(&unwatch);return t;}();
const char save_identity[]="FrostyLocalSaveProvider";
HRESULT WINAPI save_init(Api*,void* h,const char* scid,BOOLEAN,void** out){
    return valid(h)?local_save::initialize(profile().id,scid,out):E_HANDLE;
}
struct PendingSave {void* provider;};
HRESULT WINAPI save_data(unsigned op,const ProviderData* d){
    auto* pending=static_cast<PendingSave*>(d->context);
    if(op==2){
        if(!d->buffer || d->size<sizeof(void*))return E_INVALIDARG;
        std::memcpy(d->buffer,&pending->provider,sizeof(void*));pending->provider=nullptr;
    } else if(op==4){local_save::close_provider(nullptr,pending->provider);delete pending;}
    return S_OK;
}
HRESULT WINAPI save_async(Api* self,void* h,const char* scid,BOOLEAN demand,void* block){
    auto a=threading();if(!a || !block)return E_INVALIDARG;
    auto* pending=new(std::nothrow) PendingSave{};if(!pending)return E_OUTOFMEMORY;
    auto hr=save_init(self,h,scid,demand,&pending->provider);
    if(FAILED(hr)){delete pending;return hr;}
    hr=method<HRESULT(WINAPI*)(Api*,void*,void*,const void*,const char*,decltype(&save_data))>(a,7)
        (a,block,pending,save_identity,save_identity,save_data);
    if(SUCCEEDED(hr))method<void(WINAPI*)(Api*,void*,HRESULT,SIZE_T)>(a,10)(a,block,S_OK,sizeof(void*));
    else {local_save::close_provider(nullptr,pending->provider);delete pending;}
    return hr;
}
HRESULT WINAPI save_result(Api*,void* block,void** out){
    auto a=threading();if(!a || !out)return E_INVALIDARG;
    *out=nullptr;return method<HRESULT(WINAPI*)(Api*,void*,const void*,SIZE_T,void*,SIZE_T*)>(a,11)
        (a,block,save_identity,sizeof(void*),out,nullptr);
}
const auto saves=[]{
    auto t=base_table<30>();t[3]=slot(&save_init);t[4]=slot(&save_async);t[5]=slot(&save_result);t[6]=slot(&local_save::close_provider);
    t[10]=slot(&local_save::remove);t[13]=slot(&local_save::info);t[14]=slot(&local_save::enumerate);
    t[15]=slot(&local_save::enumerate_by);t[16]=slot(&local_save::create);t[17]=slot(&local_save::close);
    t[18]=slot(&local_save::blobs);t[19]=slot(&local_save::blobs_by);t[20]=slot(&local_save::read);
    t[23]=slot(&local_save::update);t[24]=slot(&local_save::close_update);t[25]=slot(&local_save::write);
    t[26]=slot(&local_save::erase);t[27]=slot(&local_save::submit);return t;
}();
const auto features=[]{
    auto t=base_table<4>();t[3]=slot(+[](Api*,UINT f)->BOOLEAN{
        return f==2||f==3||f==7||f==10||f==16||f==17||f==18;});return t;
}();
Api user{user2.data()}, tag{tags.data()}, device{devices.data()},network{networks.data()},save{saves.data()},feature{features.data()};
HRESULT WINAPI query_iface(Api* self,REFIID id,void** o){
    if(!o)return E_POINTER;
    *o=nullptr;
    if(id==__uuidof(IUnknown))*o=self==&tag?&user:self;
    else if(self==&save){if(id==save_id)*o=&save;}
    else if(self==&feature){if(id==feature_id)*o=&feature;}
    else if(self==&network){if(id==network_id||id==network2_id)*o=&network;}
    else if(self==&device){if(id==device_id || id==device2_id)*o=&device;}
    else if(id==tag_id)*o=&tag;
    else if(id==user2_id || id==user_id)*o=&user;
    return *o?S_OK:E_NOINTERFACE;
}
}
extern "C" HRESULT WINAPI QueryApiImpl(REFCLSID cls,REFIID id,void** out){
    if(cls==user_id)return query_iface(&user,id,out);
    if(cls==device_id)return query_iface(&device,id,out);
    if(cls==network_id)return query_iface(&network,id,out);
    if(cls==save_id)return query_iface(&save,id,out);
    if(cls==feature_id)return query_iface(&feature,id,out);
    auto q=core();return q?q(cls,id,out):HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}
