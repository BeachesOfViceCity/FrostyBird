// GameInput v0 (GDK 210604). The same provider serves any Xbox client.
// WineBus/SDL maps the hardware; this layer consumes standard XInput slots.
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <unknwn.h>
#include "gamepad_device.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace {
using Slot=void(*)();
template<class F> Slot slot(F f){return std::bit_cast<Slot>(f);}
using namespace controller;
constexpr HRESULT no_read=static_cast<HRESULT>(0x838a0003),no_device=static_cast<HRESULT>(0x838a0002);
constexpr GUID root_iid{0x11be2a7e,0x4254,0x445a,{0x9c,0x09,0xff,0xc4,0x0f,0x00,0x69,0x18}};
constexpr GUID device_iid{0x31dd86fb,0x4c1b,0x408a,{0x86,0x8f,0x43,0x9b,0x3c,0xd4,0x71,0x25}};
constexpr GUID reading_iid{0x2156947a,0xe1fa,0x4de0,{0xa3,0x0b,0xd8,0x12,0x93,0x1d,0xbd,0x8d}};
struct Obj {const Slot* vt;const GUID* iid;};
struct Device:Obj {
    unsigned seat;Identity identity;String name{};DeviceInfo info;
    std::atomic<UINT> status{};Pad last{};UINT64 time{},seq{};
    Device(const Slot* slots,unsigned n,Identity i):Obj{slots,&device_iid},seat(n),identity(i),info(describe(n,i,name)){}
};
struct Snapshot {Pad value;UINT64 time,sequence;Device* device;};
struct Reading : Obj {std::atomic<ULONG> refs{1};Snapshot data{};};
UINT64 timestamp(){LARGE_INTEGER f{},n{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&n);
    return UINT64(n.QuadPart/f.QuadPart)*1000000+UINT64(n.QuadPart%f.QuadPart)*1000000/f.QuadPart;}
std::mutex inputs;
std::list<Device> catalogue; // interned per seat/capability set; ABI pointers remain valid after unplug
std::array<Device*,XUSER_MAX_COUNT> active{};
std::deque<Snapshot> samples;
struct Replay {
    HANDLE handle{};const volatile LONG* value{};
    Replay(){
        wchar_t name[128]{};if(!GetEnvironmentVariableW(L"FROSTY_PAD_MAPPING",name,128))return;
        handle=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4,name);
        if(handle)value=static_cast<const volatile LONG*>(MapViewOfFile(handle,FILE_MAP_READ,0,0,4));
    }
    ~Replay(){if(value)UnmapViewOfFile(const_cast<LONG*>(value));if(handle)CloseHandle(handle);}
};
Replay& replay(){static Replay r;return r;}
UINT navigation(){
    UINT buttons=0;
    constexpr int keys[]{VK_TAB,VK_BACK,VK_RETURN,VK_ESCAPE,'X','Y',VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,'Q','E'};
    for(unsigned i=0;i<std::size(keys);++i)if(GetAsyncKeyState(keys[i])&0x8000)buttons|=1u<<i;
    if(GetAsyncKeyState(VK_SPACE)&0x8000)buttons|=4;
    // Optional one-word shared pad state for a frontend/replay on a private display.
    auto* extra=replay().value;
    if(extra)buttons|=static_cast<UINT>(*extra)&0x3fff;
    return buttons;
}
ULONG WINAPI retain(Obj*){return 2;}
ULONG WINAPI radd(Reading* r){return ++r->refs;}
ULONG WINAPI rdel(Reading* r){auto n=--r->refs;if(!n)delete r;return n;}
HRESULT WINAPI qi(Obj* obj,REFIID g,void** o){
    if(!o)return E_POINTER;
    *o=nullptr;if(g!=__uuidof(IUnknown) && g!=*obj->iid)return E_NOINTERFACE;
    if(g==reading_iid || obj->iid==&reading_iid)radd(static_cast<Reading*>(obj));
    *o=obj;return S_OK;
}
HRESULT WINAPI unsupported(Obj*,...){return E_NOTIMPL;}
UINT WINAPI zero(Obj*,...){return 0;}
template<size_t N> auto table(){std::array<Slot,N> t;t.fill(slot(&unsupported));t[0]=slot(&qi);t[1]=t[2]=slot(&retain);return t;}
void refresh(bool force=false);
const auto dt=[]{
    auto t=table<24>();t[3]=slot(+[](Device* d){return &d->info;});
    t[4]=slot(+[](Device* d){std::lock_guard guard(inputs);refresh();return d->status.load();});
    t[5]=slot(+[](Obj*,void* data){if(data){std::memset(data,0,16);static_cast<INT*>(data)[4]=-1;}});
    t[7]=slot(&zero);t[10]=slot(+[](Device* d,const Rumble* v){
        std::lock_guard guard(inputs);refresh();
        if(active[d->seat]!=d || !d->identity.physical || !d->info.rumble)return;
        XINPUT_VIBRATION r{};if(v){r.wLeftMotorSpeed=motor(v->low);r.wRightMotorSpeed=motor(v->high);}
        XInputSetState(d->seat,&r);});
    return t;
}();
const auto rt=[]{
    auto t=table<26>();std::fill(t.begin()+3,t.end(),slot(&zero));t[1]=slot(&radd);t[2]=slot(&rdel);
    t[3]=slot(+[](Reading*)->UINT{return pad;});
    t[4]=slot(+[](Reading* r,UINT){return r->data.sequence;});t[5]=slot(+[](Reading* r){return r->data.time;});
    t[6]=slot(+[](Reading* r,Obj** out){if(out)*out=r->data.device;});
    t[22]=slot(+[](Reading* r,Pad* out)->bool{if(!out)return false;*out=r->data.value;return true;});
    return t;
}();
using DeviceCallback=void(WINAPI*)(UINT64,void*,Obj*,UINT64,UINT,UINT);
using ReadingCallback=void(WINAPI*)(UINT64,void*,Reading*,bool);
struct Event {Snapshot s;UINT now{},before{};};
struct Registration {
    UINT64 id{};Obj* filter{};void* context{};DeviceCallback device{};ReadingCallback reading{};
    UINT status{};float threshold{};bool paused{},stopped{},overrun{};unsigned invoking{};
    std::array<Pad,XUSER_MAX_COUNT> delivered{};std::array<bool,XUSER_MAX_COUNT> seen{};
    std::deque<Event> queue;
};
using Reg=std::shared_ptr<Registration>;
std::map<UINT64,Reg> registrations;
std::condition_variable changed;bool running=false;
thread_local UINT64 current_callback{};
Reading* make_reading(Snapshot s){
    auto* r=new(std::nothrow) Reading;
    if(r){r->vt=rt.data();r->iid=&reading_iid;r->data=s;}
    return r;
}
bool known(Obj* p){return !p || std::any_of(catalogue.begin(),catalogue.end(),[&](Device& d){return &d==p;});}
void notify(Event event,bool connection){
    for(auto& [id,r]:registrations){
        if(r->stopped || (r->filter && r->filter!=event.s.device))continue;
        if(connection){if(!r->device || !(r->status&(event.now^event.before)))continue;}
        else{
            if(!r->reading)continue;
            unsigned i=event.s.device->seat;auto &prev=r->delivered[i],&v=event.s.value;
            if(r->seen[i] && prev.buttons==v.buttons && r->threshold>0 &&
               std::max({std::abs(prev.lt-v.lt),std::abs(prev.rt-v.rt),std::abs(prev.lx-v.lx),
                         std::abs(prev.ly-v.ly),std::abs(prev.rx-v.rx),std::abs(prev.ry-v.ry)})<r->threshold)continue;
            prev=v;r->seen[i]=true;
        }
        if(r->queue.size()==256){r->queue.pop_front();r->overrun=true;}
        r->queue.push_back(event);changed.notify_all();
    }
}
Device* intern(unsigned seat,Identity identity){
    for(auto& d:catalogue)if(d.seat==seat && d.identity==identity)return &d;
    return &catalogue.emplace_back(dt.data(),seat,identity);
}
void refresh(bool force){
    // The input mutex also serializes transitions against rumble routing.
    static UINT64 last_scan{},discover_after{},identify_after{},sequence{};
    auto now=timestamp();if(!force && last_scan && now-last_scan<1000)return;last_scan=now;
    bool discover=force || now>=discover_after,ident=force || now>=identify_after;
    if(discover)discover_after=now+100000;
    if(ident)identify_after=now+2000000;
    std::array<Device*,XUSER_MAX_COUNT> next{};
    std::array<Pad,XUSER_MAX_COUNT> states{};
    for(unsigned i=0;i<next.size();++i){
        auto* old=active[i];bool physical=old && old->identity.physical;
        if(!physical && !discover)continue;
        XINPUT_STATE x{};if(XInputGetState(i,&x)!=ERROR_SUCCESS)continue;
        next[i]=physical && !ident?old:intern(i,identify(i));states[i]=convert(x.Gamepad);
    }
    auto primary=std::find_if(next.begin(),next.end(),[](auto* p){return p!=nullptr;});
    if(primary==next.end()){next[0]=intern(0,{});primary=next.begin();}
    states[primary-next.begin()].buttons|=navigation();
    for(unsigned i=0;i<next.size();++i){
        auto *old=active[i],*d=next[i];bool attaching=old!=d;
        if(attaching && old){
            UINT before=old->status.exchange(0);
            notify({{old->last,now,old->seq,old},0,before},true);
        }
        active[i]=d;if(!d)continue;
        if(attaching){
            UINT current=3u|(d->info.rumble?4u:0u)|(d->identity.wireless?128u:0u);
            d->status=current;notify({{states[i],now,0,d},current,0},true);
        }
        auto& value=states[i];
        if(!attaching && !std::memcmp(&value,&d->last,sizeof(value)))continue;
        static const bool trace=GetEnvironmentVariableW(L"XS_CONTROLLER_TRACE",nullptr,0)!=0;
        if((trace || replay().value) && (attaching || value.buttons!=d->last.buttons)){
            char msg[128];std::snprintf(msg,sizeof(msg),"frosty/input: controller %u buttons %#x (%04x:%04x)\n",
                                        i,value.buttons,d->info.vid,d->info.pid);OutputDebugStringA(msg);
        }
        d->last=value;d->time=now;d->seq=++sequence;
        samples.push_back({value,now,d->seq,d});if(samples.size()>512)samples.pop_front();
        notify({samples.back()},false);
    }
}
HRESULT get(UINT kind,Obj* dev,Reading** out,Reading* reference,int direction){
    if(!out)return E_POINTER;
    *out=nullptr;if(!(kind&pad))return no_read;
    std::lock_guard guard(inputs);refresh();
    if(!known(dev))return E_INVALIDARG;
    Snapshot sample{};
    if(!direction){
        for(auto* d:active)if(d && (!dev || dev==d) && d->seq>sample.sequence)sample={d->last,d->time,d->seq,d};
    }else{
        if(!reference || reference->vt!=rt.data())return E_INVALIDARG;
        auto seq=reference->data.sequence;
        if(direction>0){
            for(auto& s:samples)if(s.sequence>seq && (!dev || dev==s.device)){sample=s;break;}
        }else for(auto it=samples.rbegin();it!=samples.rend();++it)
            if(it->sequence<seq && (!dev || dev==it->device)){sample=*it;break;}
    }
    if(!sample.device)return no_read;
    *out=make_reading(sample);return *out?S_OK:E_OUTOFMEMORY;
}
// Invoke guest code with no provider lock held. Unregister waits for in-flight
// calls, including blocking enumeration; callbacks can query the same device.
void invoke(std::unique_lock<std::mutex>& lock,const Reg& r,Event e){
    ++r->invoking;bool overrun=std::exchange(r->overrun,false);
    auto prior=std::exchange(current_callback,r->id);lock.unlock();
    if(r->device)r->device(r->id,r->context,e.s.device,e.s.time,e.now,e.before);
    else if(auto* s=make_reading(e.s)){r->reading(r->id,r->context,s,overrun);rdel(s);}
    lock.lock();current_callback=prior;--r->invoking;changed.notify_all();
}
DWORD WINAPI poll_callbacks(void* module){
    {
        std::unique_lock lock(inputs);
        for(;;){
            if(std::none_of(registrations.begin(),registrations.end(),[](auto& p){return !p.second->stopped;}))break;
            refresh();
            auto it=std::find_if(registrations.begin(),registrations.end(),[](auto& p){return !p.second->paused && !p.second->queue.empty();});
            if(it==registrations.end()){
                bool readings=std::any_of(registrations.begin(),registrations.end(),[](auto& p){return !p.second->stopped && p.second->reading;});
                changed.wait_for(lock,std::chrono::milliseconds(readings?4:50));continue;
            }
            auto r=it->second;auto e=r->queue.front();r->queue.pop_front();invoke(lock,r,e);
        }
        running=false;
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(module),0);
}
HRESULT subscribe(const Reg& r,UINT enumeration,UINT64* id){
    static UINT64 next=0;*id=0;std::unique_lock lock(inputs);refresh(true);
    if(!known(r->filter))return E_INVALIDARG;
    if(!running){
        HMODULE module{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<const wchar_t*>(&poll_callbacks),&module))
            return HRESULT_FROM_WIN32(GetLastError());
        auto thread=CreateThread(nullptr,0,poll_callbacks,module,0,nullptr);
        if(!thread){auto hr=HRESULT_FROM_WIN32(GetLastError());FreeLibrary(module);return hr;}
        CloseHandle(thread);running=true;
    }
    *id=r->id=++next;registrations.emplace(r->id,r);
    if(enumeration){
        for(auto* d:active)if(d && (!r->filter || r->filter==d) && (r->status&d->status.load()))
            r->queue.push_back({{d->last,d->time,d->seq,d},d->status.load(),0});
        if(enumeration==2){
            r->paused=true;auto initial=std::move(r->queue);r->queue.clear();
            for(auto& e:initial){if(r->stopped)break;invoke(lock,r,e);}
            r->paused=false;
        }
    }
    changed.notify_all();return S_OK;
}
HRESULT WINAPI enumerate(Obj*,Obj* dev,UINT kind,UINT status,UINT enumeration,void* context,DeviceCallback cb,UINT64* id) {
    if(!id || !cb || enumeration>2)return E_INVALIDARG;
    auto r=std::make_shared<Registration>();r->device=cb;r->context=context;r->filter=dev;r->status=(kind&pad)?status:0;
    return subscribe(r,enumeration,id);
}
HRESULT WINAPI on_reading(Obj*,Obj* dev,UINT kind,float threshold,void* context,ReadingCallback cb,UINT64* id){
    if(!id || !cb || !std::isfinite(threshold) || threshold<0)return E_INVALIDARG;
    if(!(kind&pad))return E_NOTIMPL;
    auto r=std::make_shared<Registration>();r->reading=cb;r->context=context;r->filter=dev;r->threshold=threshold;
    return subscribe(r,0,id);
}
bool stop(UINT64 id,bool remove,UINT64 timeout){
    std::unique_lock lock(inputs);if(id==UINT64_MAX)id=current_callback;
    auto it=registrations.find(id);if(it==registrations.end())return false;
    auto r=it->second;r->stopped=true;r->queue.clear();changed.notify_all();
    if(!remove)return true;
    if(current_callback!=id && r->invoking){
        if(timeout==UINT64_MAX)changed.wait(lock,[&]{return !r->invoking;});
        else if(!changed.wait_for(lock,std::chrono::microseconds(std::min<UINT64>(timeout,INT64_MAX/1000)),[&]{return !r->invoking;}))return false;
    }
    registrations.erase(id);return true;
}
const auto root_slots=[]{
    auto t=table<24>();t[3]=slot(+[](Obj*){return timestamp();});
    t[4]=slot(+[](Obj*,UINT k,Obj* d,Reading** r){return get(k,d,r,nullptr,0);});
    t[5]=slot(+[](Obj*,Reading* ref,UINT k,Obj* d,Reading** r){return get(k,d,r,ref,1);});
    t[6]=slot(+[](Obj*,Reading* ref,UINT k,Obj* d,Reading** r){return get(k,d,r,ref,-1);});
    t[8]=slot(&on_reading);t[9]=slot(&enumerate);
    t[12]=slot(+[](Obj*,UINT64 id){stop(id,false,0);});
    t[13]=slot(+[](Obj*,UINT64 id,UINT64 us){return stop(id,true,us);});
    t[15]=slot(+[](Obj*,UINT k,Obj** o)->HRESULT{if(!o)return E_POINTER;*o=nullptr;
        std::lock_guard g(inputs);refresh();if(k==pad)for(auto* d:active)if(d){*o=d;break;}
        return *o?S_OK:no_device;});
    t[16]=slot(+[](Obj*,const APP_LOCAL_DEVICE_ID* id,Obj** o)->HRESULT{if(!o)return E_POINTER;
        *o=nullptr;std::lock_guard g(inputs);refresh();
        if(id)for(auto* d:active)if(d && !std::memcmp(id,&d->info.id,sizeof(*id))){*o=d;break;}
        return *o?S_OK:no_device;});
    return t;
}();
Obj root{root_slots.data(),&root_iid};
}
extern "C" __declspec(dllexport) HRESULT WINAPI GameInputCreate(void** out){
    if(!out)return E_POINTER;
    replay();*out=&root;return S_OK;
}
