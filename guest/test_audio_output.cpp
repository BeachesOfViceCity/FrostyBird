// Standalone XAudio2.9 consumer. Uses only the public PE interface and the
// default render endpoint. A private sink monitor checks the resulting mix.
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
void check(HRESULT hr,const char* op){if(FAILED(hr)){std::fprintf(stderr,"%s: %#lx\n",op,(unsigned long)hr);throw std::runtime_error(op);}}
struct Callback final:IXAudio2VoiceCallback{
    HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<unsigned> ended{},errors{};
    ~Callback(){if(done)CloseHandle(done);}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32)override{}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd()override{}
    void STDMETHODCALLTYPE OnStreamEnd()override{SetEvent(done);}
    void STDMETHODCALLTYPE OnBufferStart(void*)override{}
    void STDMETHODCALLTYPE OnBufferEnd(void* context)override{
        if(reinterpret_cast<UINT_PTR>(context)!=ended.fetch_add(1)+1)++errors;
    }
    void STDMETHODCALLTYPE OnLoopEnd(void*)override{}
    void STDMETHODCALLTYPE OnVoiceError(void*,HRESULT)override{++errors;SetEvent(done);}
};
struct Voices{
    IXAudio2MasteringVoice* master{};IXAudio2SourceVoice* source{};
    ~Voices(){if(source)source->DestroyVoice();if(master)master->DestroyVoice();}
};
}
#include "test_audio_aac.hpp"
int main(int argc,char** argv)try{
    bool console=argc>1 && std::strcmp(argv[1],"--console")==0;
    if(console){--argc;++argv;}
    if(argc>2)return 2;
    check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"CoInitializeEx");
    struct Com{~Com(){CoUninitialize();}} com;
    auto module=LoadLibraryW(L"xaudio2_9.dll");if(!module)return 2;
    using Create=HRESULT(WINAPI*)(IXAudio2**,UINT32,XAUDIO2_PROCESSOR);
    auto create=std::bit_cast<Create>(GetProcAddress(module,MAKEINTRESOURCEA(1)));if(!create)return 2;
    Microsoft::WRL::ComPtr<IXAudio2> engine;
    DWORD_PTR process{},system{};GetProcessAffinityMask(GetCurrentProcess(),&process,&system);
    check(create(engine.GetAddressOf(),console?0x400:0,console?XAUDIO2_PROCESSOR(process):XAUDIO2_DEFAULT_PROCESSOR),"XAudio2Create ordinal 1");
    UINT rate=48000,channels=2;DWORD mask=3;
    std::array<std::vector<float>,4> samples;
    if(argc==2){
        auto pcm=decode_aac(argv[1],channels,rate,mask);
        for(size_t k=0;k<samples.size();++k){
            auto at=[&](size_t part){return pcm.begin()+(pcm.size()/channels*part/samples.size())*channels;};
            samples[k].assign(at(k),at(k+1));
        }
    }else for(unsigned k=0;k<samples.size();++k){
        UINT n=k==0||k==3?rate/4:rate;samples[k].resize(2*n);
        if(k==1||k==2)for(UINT i=0;i<n;++i)samples[k][2*i+k-1]=float(.15*std::sin(2*std::numbers::pi*(k==1?1000:1500)*i/rate));
    }
    Callback cb;Voices v;if(!cb.done)return 2;
    check(engine->CreateMasteringVoice(&v.master,2,rate,0,nullptr,nullptr,AudioCategory_GameEffects),"CreateMasteringVoice");
    WAVEFORMATEXTENSIBLE fmt{};fmt.Format={WAVE_FORMAT_EXTENSIBLE,WORD(channels),rate,rate*channels*4,WORD(channels*4),32,22};
    fmt.Samples.wValidBitsPerSample=32;fmt.dwChannelMask=mask;fmt.SubFormat=MFAudioFormat_Float;
    if(channels<=2){fmt.Format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;fmt.Format.cbSize=0;}
    check(engine->CreateSourceVoice(&v.source,&fmt.Format,console?XAUDIO2_VOICE_NOPITCH:0,2,&cb,nullptr,nullptr),"CreateSourceVoice");
    UINT64 frames=0;
    for(unsigned k=0;k<samples.size();++k){
        frames+=samples[k].size()/channels;
        XAUDIO2_BUFFER b{};b.AudioBytes=UINT(samples[k].size()*4);b.pAudioData=reinterpret_cast<const BYTE*>(samples[k].data());
        b.pContext=reinterpret_cast<void*>(UINT_PTR(k+1));if(k==3)b.Flags=XAUDIO2_END_OF_STREAM;
        check(v.source->SubmitSourceBuffer(&b),"SubmitSourceBuffer");
    }
    check(engine->StartEngine(),"StartEngine");check(v.source->Start(),"Start");
    if(WaitForSingleObject(cb.done,10000)!=WAIT_OBJECT_0 || cb.errors || cb.ended!=4)return 3;
    XAUDIO2_VOICE_STATE state{};v.source->GetState(&state);if(state.BuffersQueued)return 4;
    std::printf("XAudio2.9: %llu frames (%u channels) submitted at %u Hz, 4 ordered buffer callbacks and end of stream\n",frames,channels,rate);
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"audio output check: %s\n",e.what());return 1;}
