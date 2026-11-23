// GameInput v0 device metadata and the XInput -> normalized gamepad contract.
#pragma once
#include "controller_seat.hpp"
#include <xinput.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>

namespace controller {
constexpr UINT pad=0x40000;
struct Pad {UINT buttons;float lt,rt,lx,ly,rx,ry;};
struct Rumble {float low,high,left,right;};
struct String {UINT bytes,points;const char* data;};
struct DeviceInfo {
    UINT size;WORD vid,pid,revision;BYTE iface,collection;WORD page,usage;
    WORD hardware[4],firmware[4];APP_LOCAL_DEVICE_ID id,root;
    INT family;UINT caps,input,rumble,counts[11];UINT descriptor_size;
    const void* info[20];UINT system_buttons;
};
static_assert(offsetof(DeviceInfo,input)==0x68 && offsetof(DeviceInfo,system_buttons)==0x140);
struct Identity {
    bool physical{};WORD vid{},pid{},rev{};UINT motors{};bool wireless{};
    bool operator==(const Identity&)const=default;
};
inline Identity identify(DWORD index){
    struct Extended {XINPUT_CAPABILITIES caps;WORD vendor,product,revision,reserved;DWORD extra;};
    using CapsEx=DWORD(WINAPI*)(DWORD,DWORD,DWORD,Extended*);
    static auto ext=std::bit_cast<CapsEx>(GetProcAddress(GetModuleHandleW(L"xinput1_4.dll"),MAKEINTRESOURCEA(108)));
    Extended e{};
    if(!ext || ext(1,index,0,&e)!=ERROR_SUCCESS)XInputGetCapabilities(index,0,&e.caps);
    return {true,e.vendor,e.product,e.revision,
            UINT(!!e.caps.Vibration.wLeftMotorSpeed)|(UINT(!!e.caps.Vibration.wRightMotorSpeed)<<1),
            (e.caps.Flags&XINPUT_CAPS_WIRELESS)!=0};
}
inline DeviceInfo describe(unsigned seat,Identity i,String& name){
    const bool ps5=i.vid==0x054c && (i.pid==0x0ce6 || i.pid==0x0df2);
    // GameInputLabel values, in GameInputGamepadInfo field order. Labels
    // describe the hardware artwork; the gamepad button bits stay positional.
    static constexpr UINT xbox[]={4,5,7,8,9,10,11,12,13,14,15,18,17,20};
    static constexpr UINT sony[]={103,104,78,79,80,81,83,84,85,86,112,118,114,120};
    const char* text=ps5?(i.pid==0x0df2?"DualSense Edge":"DualSense Wireless Controller"):
                     i.physical?"XInput gamepad":"Keyboard gamepad";
    name={UINT(std::strlen(text)),UINT(std::strlen(text)),text};
    DeviceInfo d{};d.size=sizeof(d);d.vid=i.vid;d.pid=i.pid;d.revision=i.rev;d.page=1;d.usage=5;
    d.id=controller_seat_id(seat);d.root=d.id;d.family=i.physical?3:-1;d.input=pad;d.rumble=i.motors;
    d.caps=i.wireless?0x10:0;d.info[12]=ps5?sony:xbox;d.info[17]=&name;
    return d;
}
inline Pad convert(const XINPUT_GAMEPAD& x){
    constexpr WORD map[]{XINPUT_GAMEPAD_START,XINPUT_GAMEPAD_BACK,XINPUT_GAMEPAD_A,XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,XINPUT_GAMEPAD_Y,XINPUT_GAMEPAD_DPAD_UP,XINPUT_GAMEPAD_DPAD_DOWN,XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_RIGHT,XINPUT_GAMEPAD_LEFT_SHOULDER,XINPUT_GAMEPAD_RIGHT_SHOULDER,XINPUT_GAMEPAD_LEFT_THUMB,XINPUT_GAMEPAD_RIGHT_THUMB};
    auto axis=[](SHORT a){return a<0?a/32768.f:a/32767.f;};
    Pad v{0,x.bLeftTrigger/255.f,x.bRightTrigger/255.f,axis(x.sThumbLX),axis(x.sThumbLY),axis(x.sThumbRX),axis(x.sThumbRY)};
    for(unsigned i=0;i<std::size(map);++i)if(x.wButtons&map[i])v.buttons|=1u<<i;
    return v;
}
inline WORD motor(float value){return std::isfinite(value)?WORD(std::lround(std::clamp(value,0.f,1.f)*65535)):0;}
}
