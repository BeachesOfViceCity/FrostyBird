// Read a small ADTS fixture, feed its raw access units and AudioSpecificConfig
// through the system AAC MFT, and return the actual negotiated float samples.
#pragma once
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <fstream>
#include <iterator>
#include <algorithm>

namespace {
std::vector<float> decode_aac(const char* path, UINT& channels, UINT& rate, DWORD& mask) {
    using Microsoft::WRL::ComPtr;
    std::ifstream f(path,std::ios::binary);
    std::vector<BYTE> data(std::istreambuf_iterator<char>(f),{});
    if(data.size()<7 || data.size()>8*1024*1024)throw std::runtime_error("invalid ADTS fixture size");
    unsigned profile=1+(data[2]>>6),idx=(data[2]>>2)&15;
    constexpr UINT rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    if(idx>=std::size(rates))throw std::runtime_error("invalid AAC sample rate");
    rate=rates[idx];channels=((data[2]&1)<<2)|(data[3]>>6);
    BYTE asc[]={BYTE((profile<<3)|(idx>>1)),BYTE(((idx&1)<<7)|(channels<<3))};
    constexpr GUID cls{0x32d186a7,0x218f,0x4c75,{0x88,0x76,0xdd,0x77,0x27,0x3a,0x89,0x99}};
    check(MFStartup(MF_VERSION),"MFStartup");struct Stop{~Stop(){MFShutdown();}}stop;
    ComPtr<IMFTransform> m;check(CoCreateInstance(cls,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&m)),"CMSAACDecMFT");
    ComPtr<IMFMediaType> in;check(MFCreateMediaType(&in),"input type");
    auto raw=MFAudioFormat_Base;raw.Data1=0xff;
    check(in->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),"audio type");
    check(in->SetGUID(MF_MT_SUBTYPE,raw),"raw AAC");
    check(in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,0),"payload");
    check(in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,rate),"rate");
    check(in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,channels),"channels");
    check(in->SetBlob(MF_MT_USER_DATA,asc,2),"ASC");
    check(m->SetInputType(0,in.Get(),0),"SetInputType");
    auto negotiate=[&]{
        ComPtr<IMFMediaType> out;check(m->GetOutputAvailableType(0,0,&out),"GetOutputAvailableType");
        GUID format{};UINT32 n{},hz{};
        check(out->GetGUID(MF_MT_SUBTYPE,&format),"output format");
        check(out->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,&n),"output channels");
        check(out->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,&hz),"output rate");
        if(format!=MFAudioFormat_Float || n!=channels || hz!=rate)throw std::runtime_error("wrong PCM format");
        UINT32 layout{};out->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK,&layout);mask=layout;
        check(m->SetOutputType(0,out.Get(),0),"SetOutputType");
    };
    negotiate();m->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0);m->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0);
    auto sample=[](DWORD size,ComPtr<IMFMediaBuffer>& b){
        ComPtr<IMFSample> s;check(MFCreateSample(&s),"sample");check(MFCreateMemoryBuffer(size,&b),"buffer");
        check(s->AddBuffer(b.Get()),"AddBuffer");return s;
    };
    std::vector<float> pcm;
    auto receive=[&]{
        for(unsigned k=0;k<128;++k){
            MFT_OUTPUT_STREAM_INFO info{};check(m->GetOutputStreamInfo(0,&info),"GetOutputStreamInfo");
            ComPtr<IMFMediaBuffer> b;auto s=sample(info.cbSize,b);MFT_OUTPUT_DATA_BUFFER o{0,s.Get(),0,nullptr};
            DWORD status{};HRESULT hr=m->ProcessOutput(0,1,&o,&status);if(o.pEvents)o.pEvents->Release();
            if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT)return;
            if(hr==MF_E_TRANSFORM_STREAM_CHANGE){negotiate();continue;}
            check(hr,"ProcessOutput");BYTE* ptr{};DWORD n{};check(b->Lock(&ptr,nullptr,&n),"lock output");
            if(n%(4*channels)){b->Unlock();throw std::runtime_error("unaligned PCM");}
            pcm.insert(pcm.end(),reinterpret_cast<float*>(ptr),reinterpret_cast<float*>(ptr)+n/4);
            check(b->Unlock(),"unlock output");
        }
        throw std::runtime_error("AAC output made no progress");
    };
    unsigned units=0;
    for(size_t pos=0;pos<data.size();){
        auto* p=data.data()+pos;
        if(data.size()-pos<7 || p[0]!=0xff || (p[1]&0xf6)!=0xf0)throw std::runtime_error("invalid ADTS header");
        unsigned n=((p[3]&3)<<11)|(p[4]<<3)|(p[5]>>5),h=(p[1]&1)?7:9;
        if(n<=h || n>data.size()-pos || (p[6]&3))throw std::runtime_error("invalid ADTS access unit");
        ComPtr<IMFMediaBuffer> b;auto s=sample(n-h,b);BYTE* ptr{};check(b->Lock(&ptr,nullptr,nullptr),"lock input");
        std::memcpy(ptr,p+h,n-h);b->Unlock();check(b->SetCurrentLength(n-h),"input length");
        check(s->SetSampleTime(LONGLONG(units)*1024*10000000/rate),"timestamp");
        check(s->SetSampleDuration(1024LL*10000000/rate),"duration");
        check(m->ProcessInput(0,s.Get(),0),"ProcessInput");receive();++units;pos+=n;
    }
    m->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM,0);check(m->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0),"drain");receive();
    if(pcm.size()<channels*rate/2)throw std::runtime_error("short decoded AAC");
    for(unsigned ch=0;ch<channels;++ch){
        double sum=0;for(size_t i=ch;i<pcm.size();i+=channels)sum+=pcm[i]*pcm[i];
        if(!std::isfinite(sum) || sum*channels/pcm.size()<1e-7)throw std::runtime_error("silent or invalid AAC fixture channel");
    }
    std::printf("AAC MFT: %u raw access units, %zu float frames, %u channels, %u Hz, mask %#lx\n",
                units,pcm.size()/channels,channels,rate,mask);return pcm;
}
}
