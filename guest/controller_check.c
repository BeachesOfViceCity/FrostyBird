/* Standalone consumer of Wine's XInput and the public GameInput v0 ABI. */
#define COBJMACROS
#include <windows.h>
#include <xinput.h>
#include "xs-test-gameinput.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct caps_ex { XINPUT_CAPABILITIES caps; WORD vid,pid,rev,reserved; DWORD reserved2; };
typedef DWORD (WINAPI *get_caps_ex)(DWORD,DWORD,DWORD,struct caps_ex*);
typedef HRESULT (WINAPI *create_input)(v0_IGameInput**);
static unsigned vendor,product;
static v0_IGameInputDevice* found;
static unsigned arrivals,removals;
static unsigned callbacks;
static CRITICAL_SECTION lock;

static void WINAPI device_event(uint64_t token,void* ctx,v0_IGameInputDevice* d,uint64_t t,GameInputDeviceStatus now,GameInputDeviceStatus before){
    (void)token;(void)ctx;(void)t;
    const v0_GameInputDeviceInfo* i=v0_IGameInputDevice_GetDeviceInfo(d);
    EnterCriticalSection(&lock);
    printf("GameInput %04x:%04x %s, status %#x, rumble %#x, %s\n",i->vendorId,i->productId,
           i->displayName?i->displayName->data:"(unnamed)",now,i->supportedRumbleMotors,
           (now&GameInputDeviceConnected)?"connected":"disconnected");
    if(i->vendorId==vendor && i->productId==product){
        if(now&GameInputDeviceConnected){
            if(found!=d){if(found)v0_IGameInputDevice_Release(found);v0_IGameInputDevice_AddRef(d);found=d;}
            ++arrivals;
        }else if(before&GameInputDeviceConnected){++removals;}
    }
    LeaveCriticalSection(&lock);
}

static void WINAPI reading_event(uint64_t token,void* ctx,v0_IGameInputReading* r,bool overrun){
    (void)token;(void)ctx;(void)overrun;
    v0_IGameInputDevice* d=NULL;v0_IGameInputReading_GetDevice(r,&d);if(!d)return;
    const v0_GameInputDeviceInfo* i=v0_IGameInputDevice_GetDeviceInfo(d);GameInputGamepadState s;
    if(i->vendorId==vendor && i->productId==product && v0_IGameInputReading_GetGamepadState(r,&s)){
        EnterCriticalSection(&lock);++callbacks;LeaveCriticalSection(&lock);
    }
    v0_IGameInputDevice_Release(d);
}
static bool match(GameInputGamepadState a,GameInputGamepadState b){
    return a.buttons==b.buttons && fabsf(a.leftTrigger-b.leftTrigger)<.025f && fabsf(a.rightTrigger-b.rightTrigger)<.025f &&
        fabsf(a.leftThumbstickX-b.leftThumbstickX)<.035f && fabsf(a.leftThumbstickY-b.leftThumbstickY)<.035f &&
        fabsf(a.rightThumbstickX-b.rightThumbstickX)<.035f && fabsf(a.rightThumbstickY-b.rightThumbstickY)<.035f;
}
static int exercise(v0_IGameInput* input){
    GameInputCallbackToken token=0;v0_IGameInputReading* retained=NULL;GameInputGamepadState original={0};
    if(FAILED(v0_IGameInput_RegisterReadingCallback(input,NULL,GameInputKindGamepad,0,NULL,reading_event,&token)))return 4;
    char line[160];int ret=1;puts("READY");
    while(fgets(line,sizeof(line),stdin)){
        if(!strcmp(line,"quit\n")){ret=0;break;}
        ULONGLONG deadline=GetTickCount64()+6000;bool ok=false;
        unsigned n,bits;GameInputGamepadState expected={0},s={0};
        while(!ok && GetTickCount64()<deadline){
            EnterCriticalSection(&lock);
            v0_IGameInputDevice* d=found;if(d)v0_IGameInputDevice_AddRef(d);
            unsigned adds=arrivals,dels=removals,reads=callbacks;LeaveCriticalSection(&lock);
            if(sscanf(line,"attached %u",&n)==1 && d && adds==n){
                const v0_GameInputDeviceInfo* i=v0_IGameInputDevice_GetDeviceInfo(d);
                v0_IGameInputDevice* lookup=NULL;
                ok=(v0_IGameInputDevice_GetDeviceStatus(d)&GameInputDeviceConnected) && i->gamepadInfo &&
                    i->gamepadInfo->aButtonLabel==GameInputLabelIconCross && i->supportedRumbleMotors==3 &&
                    SUCCEEDED(v0_IGameInput_FindDeviceFromId(input,&i->deviceId,&lookup)) && lookup==d;
                if(lookup)v0_IGameInputDevice_Release(lookup);
            }else if(sscanf(line,"removed %u",&n)==1 && d && dels==n){
                v0_IGameInputReading* r=NULL;
                ok=!v0_IGameInputDevice_GetDeviceStatus(d) && FAILED(v0_IGameInput_GetCurrentReading(input,GameInputKindGamepad,d,&r));
                if(r)v0_IGameInputReading_Release(r);
                if(ok && retained){
                    GameInputGamepadState saved;ok=v0_IGameInputReading_GetGamepadState(retained,&saved) && match(original,saved);
                    v0_IGameInputReading_Release(retained);retained=NULL;
                }
            }else if(d && sscanf(line,"state %x %f %f %f %f %f %f",&bits,&expected.leftTrigger,&expected.rightTrigger,
                                        &expected.leftThumbstickX,&expected.leftThumbstickY,&expected.rightThumbstickX,&expected.rightThumbstickY)==7){
                expected.buttons=(GameInputGamepadButtons)bits;v0_IGameInputReading* r=NULL;
                if(SUCCEEDED(v0_IGameInput_GetCurrentReading(input,GameInputKindGamepad,d,&r))){
                    ok=v0_IGameInputReading_GetGamepadState(r,&s) && match(s,expected);
                    if(ok && !retained && bits){retained=r;original=s;v0_IGameInputReading_AddRef(r);}
                    v0_IGameInputReading_Release(r);
                }
            }else if(d && !strncmp(line,"rumble ",7)){
                GameInputRumbleParams v={0};if(sscanf(line+7,"%f %f",&v.lowFrequency,&v.highFrequency)==2){
                    v0_IGameInputDevice_SetRumbleState(d,&v);ok=true;
                }
            }else if(!strcmp(line,"history\n") && d && retained && reads>=3){
                v0_IGameInputReading *next=NULL,*prev=NULL;GameInputGamepadState saved;
                ok=v0_IGameInputReading_GetGamepadState(retained,&saved) && match(original,saved) &&
                    SUCCEEDED(v0_IGameInput_GetNextReading(input,retained,GameInputKindGamepad,d,&next)) &&
                    SUCCEEDED(v0_IGameInput_GetPreviousReading(input,next,GameInputKindGamepad,d,&prev)) &&
                    v0_IGameInputReading_GetSequenceNumber(prev,GameInputKindGamepad)==v0_IGameInputReading_GetSequenceNumber(retained,GameInputKindGamepad);
                if(prev)v0_IGameInputReading_Release(prev);
                if(next)v0_IGameInputReading_Release(next);
            }
            if(d)v0_IGameInputDevice_Release(d);
            if(!ok)Sleep(5);
        }
        if(!ok){fprintf(stderr,"failed %s(last buttons %#x, analog %.3f %.3f %.3f %.3f %.3f %.3f)\n",line,s.buttons,
                       s.leftTrigger,s.rightTrigger,s.leftThumbstickX,s.leftThumbstickY,s.rightThumbstickX,s.rightThumbstickY);break;}
        printf("OK %s",line);
    }
    if(retained)v0_IGameInputReading_Release(retained);
    v0_IGameInput_UnregisterCallback(input,token,UINT64_MAX);
    return ret;
}
static int xi_scan(void){
    get_caps_ex caps_ex=(get_caps_ex)(void*)GetProcAddress(GetModuleHandleW(L"xinput1_4.dll"),MAKEINTRESOURCEA(108));
    unsigned n=0;
    for(DWORD seat=0;seat<XUSER_MAX_COUNT;++seat){
        XINPUT_STATE s;struct caps_ex c={0};
        if(XInputGetState(seat,&s))continue;
        if(caps_ex)caps_ex(1,seat,XINPUT_FLAG_GAMEPAD,&c);
        printf("XInput slot %lu %04x:%04x packet %lu buttons %#x LT/RT %u/%u sticks %d,%d %d,%d motors %u/%u\n",
               seat,c.vid,c.pid,s.dwPacketNumber,s.Gamepad.wButtons,s.Gamepad.bLeftTrigger,s.Gamepad.bRightTrigger,
               s.Gamepad.sThumbLX,s.Gamepad.sThumbLY,s.Gamepad.sThumbRX,s.Gamepad.sThumbRY,
               c.caps.Vibration.wLeftMotorSpeed,c.caps.Vibration.wRightMotorSpeed);
        ++n;
    }
    printf("XInput connected slots: %u\n",n);
    return n?0:1;
}

int main(int argc,char** argv){
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc==2 && !strcmp(argv[1],"--xinput"))return xi_scan();
    bool test=argc==4 && !strcmp(argv[3],"--exercise");
    bool watch=argc==4 && !strcmp(argv[3],"--watch");
    if(argc!=3 && !test && !watch){fprintf(stderr,"usage: xs-controller-check --xinput | VENDOR_HEX PRODUCT_HEX [--watch | --exercise]\n");return 2;}
    vendor=strtoul(argv[1],NULL,16);product=strtoul(argv[2],NULL,16);
    xi_scan();InitializeCriticalSection(&lock);
    HMODULE m=LoadLibraryW(L"frosty-gameinput.dll");
    create_input create=m?(create_input)(void*)GetProcAddress(m,"GameInputCreate"):NULL;
    v0_IGameInput* input=NULL;GameInputCallbackToken token=0;
    if(!create || FAILED(create(&input)))return 3;
    if(FAILED(v0_IGameInput_RegisterDeviceCallback(input,NULL,GameInputKindGamepad,GameInputDeviceConnected,
              GameInputBlockingEnumeration,NULL,device_event,&token)))return 4;
    int ret=1;
    EnterCriticalSection(&lock);v0_IGameInputDevice* d=found;if(d)v0_IGameInputDevice_AddRef(d);LeaveCriticalSection(&lock);
    if(test)ret=exercise(input);
    else if(d){
        uint64_t seq=0;ULONGLONG end=GetTickCount64()+(watch?5000:0);
        do{
            v0_IGameInputReading* r=NULL;GameInputGamepadState s;
            if(SUCCEEDED(v0_IGameInput_GetCurrentReading(input,GameInputKindGamepad,d,&r)) &&
               v0_IGameInputReading_GetGamepadState(r,&s)){
                uint64_t n=v0_IGameInputReading_GetSequenceNumber(r,GameInputKindGamepad);
                if(n!=seq)printf("GameInput reading: buttons %#x, LT/RT %.3f/%.3f, sticks %.3f,%.3f %.3f,%.3f\n",
                       s.buttons,s.leftTrigger,s.rightTrigger,s.leftThumbstickX,s.leftThumbstickY,s.rightThumbstickX,s.rightThumbstickY);
                seq=n;ret=0;
            }
            if(r)v0_IGameInputReading_Release(r);
            if(watch)Sleep(10);
        }while(GetTickCount64()<end);
    }
    if(d)v0_IGameInputDevice_Release(d);
    v0_IGameInput_UnregisterCallback(input,token,UINT64_MAX);
    if(found)v0_IGameInputDevice_Release(found);
    v0_IGameInput_Release(input);DeleteCriticalSection(&lock);
    return ret;
}
