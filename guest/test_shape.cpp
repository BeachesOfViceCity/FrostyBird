// Independent PE client: no engine or title executable. Compares SHAPE's
// two-slot streaming and circular PCM against a stock file decoder oracle.
#include "shape.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
using namespace shape;
namespace {
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
void check(HRESULT hr,const char* what){if(FAILED(hr)){std::fprintf(stderr,"%s: %#lx\n",what,(unsigned long)hr);throw std::runtime_error(what);}}
struct Block {
    u32 a{};void* p{};
    Block(u32 n,u32 align,u32 type=1){check(ApuAlloc(&p,&a,n,align,type),"ApuAlloc");}
    ~Block(){ApuFree(p);}
};
std::vector<BYTE> file(const char* name){std::ifstream f(name,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
}
int main(int argc,char** argv)try{
    if(argc!=4){std::fprintf(stderr,"usage: xs-shape-check packets reference.s16 channels\n");return 2;}
    const auto data=file(argv[1]),reference=file(argv[2]);u32 channels=u32(std::atoi(argv[3]));
    require((channels==1 || channels==2) && data.size()>=4096 && data.size()%2048==0 && reference.size()>4096,"invalid fixture");
    u32 packet_count=u32(data.size()/2048);size_t expected=0;
    for(size_t i=0;i<data.size();i+=2048)expected+=data[i]>>2;
    expected*=512*channels;
    check(ApuCreateHeap(4<<20,256<<10),"create heap");HeapState baseline{},s{};
    check(ApuHeapGetState(&baseline,1),"initial state");
    {
        Block a(357,256);auto* interior=static_cast<BYTE*>(a.p)+56;
        require(!((a.a|uintptr_t(a.p))&255) && ApuMapVirtualAddress(interior)==a.a+56 &&
                ApuMapApuAddress(a.a+56)==interior && !ApuMapApuAddress(a.a+357),"interior mapping");
        require(ApuIsVirtualAddressValid(a.p,256) && !ApuIsVirtualAddressValid(interior,256),"alignment");
        void* p{};require(FAILED(ApuAlloc(&p,nullptr,4,6,1)) && FAILED(ApuAlloc(&p,nullptr,5<<20,64,1)),"heap constraints");
    }
    // Freeing the last allocation may retire an idle heap.
    if(FAILED(ApuHeapGetState(&s,1)))check(ApuCreateHeap(4<<20,256<<10),"recreate idle heap");
    check(ApuHeapGetState(&s,1),"restored state");require(s.free==baseline.free && !s.allocations,"heap accounting");
    Tables tables{};tables.count[4]=5;check(AcpHalAllocateShapeContexts(&tables),"contexts");
    require(ApuMapApuAddress(tables.apu[4])==tables.cpu[4] && !((uintptr_t(tables.cpu[4])|tables.apu[4])&63),"context table mapping");
    Hal* hal{};check(AcpHalCreate(&hal),"HAL");check(hal->connect(32,64),"connect");
    Block fence(4,4,2),in0(2048,256),in1(2048,256),out(9*256,256),work(2048,128);
    auto* word=static_cast<volatile LONG*>(fence.p);
    u32 mask=0x10;constexpr uint64_t tag=0x62220000abcdfedcULL;u32 msgs=0;
    auto command=[&](u32 op,const void* args){
        *word=0;check(hal->submit(op,tag,0xffffffff,args,fence.a),"submit");require(*word!=0,"missing fence");
        Message m{};require(hal->available() && hal->pop(&m) && (op==6 || !hal->available()) && m.type==0x10 &&
            m.tag_low==u32(tag) && m.tag_high==u32(tag>>32),"message ordering/tag");++msgs;
    };
    command(2,&mask);
    require(FAILED(AcpHalReleaseShapeContexts()) && FAILED(hal->submit(19,0,0,&mask,0)),"unsupported command accepted");
    u32 id=3;auto* ctx=static_cast<Context*>(tables.cpu[4]);auto w=ctx[id].w;
    constexpr u32 cap=9;
    w[0]=(cap<<22)|1|(1<<20);w[1]=(1u<<31)|((channels-1)<<29)|(3<<27)|(3<<20)|1;
    w[2]=32;w[5]=in0.a;w[6]=in1.a;w[7]=out.a;w[8]=work.a;w[9]=1u<<30; // one quantum per command
    std::memcpy(in0.p,data.data(),2048);
    size_t fed=1;u32 wraps=0,handoffs=0,split_waits=0;
    std::vector<short> result;
    for(size_t k=0;k<expected/128+32 && result.size()<expected;++k){
        u32 last=w[0]>>27;command(5,&id);command(7,&id);
        if(w[3]>>31)std::fprintf(stderr,"parser error at slot %u bit %u, produced %zu samples, word2=%08x\n",w[4]>>31,w[2]&0x3ffffff,result.size(),w[2]);
        require(!(w[3]>>31),"compressed frame rejected");
        auto wr=w[0]>>27;u32 available=(wr+cap-last)%cap;
        if(wr==last && !(w[1]>>31))available=cap;
        if(wr<last)++wraps;
        for(u32 j=0;j<available;++j){
            auto* src=static_cast<const short*>(out.p)+128*((last+j)%cap);
            result.insert(result.end(),src,src+128);
        }
        w[9]=(w[9]&~31u)|wr;w[1]|=1u<<31;
        auto code=(w[2]>>26)&31;
        if(code==4 || code==8){
            if(code==8)++split_waits;
            auto slot=w[4]>>31;
            if(fed<packet_count){
                std::memcpy(slot?in1.p:in0.p,data.data()+2048*fed++,2048);
                w[0]|=1u<<(20+slot);++handoffs;
            }else break;
        }
    }
    if(fed!=packet_count || !wraps || !handoffs || result.size()!=expected)
        std::fprintf(stderr,"fed %zu/%u, wraps %u, handoffs %u, waits %u, samples %zu/%zu, ctx %08x %08x %08x %08x %08x\n",
                     fed,packet_count,wraps,handoffs,split_waits,result.size(),expected,w[0],w[1],w[2],w[3],w[4]);
    require(fed==packet_count && wraps && handoffs && result.size()==expected,"stream length, wrap or handoff");
    // Stock FFmpeg's file decoder removes the first MDCT frame and its
    // 64-sample encoder delay; its flushed tail is not a SHAPE input frame.
    constexpr size_t delay=512+64;size_t start=delay*channels;
    size_t compare=std::min(reference.size()/2,result.size()-start);
    require(compare>=result.size()*9/10,"insufficient reference overlap");
    int maxerr=0,peak=0;
    for(size_t i=0;i<compare;++i){short ref;std::memcpy(&ref,reference.data()+2*i,2);
        maxerr=std::max(maxerr,std::abs(int(result[start+i])-ref));peak=std::max(peak,std::abs(int(ref)));}
    std::printf("SHAPE: %zu frames, %u packet handoffs, %u split-frame waits, %u ring wraps; compared %zu frames, peak %d, max error %d LSB\n",
                result.size()/channels,handoffs,split_waits,wraps,compare/channels,peak,maxerr);
    require(maxerr<=1 && peak>100,"PCM differs from independent file decoder");
    // Interleave an unrelated packet after each real packet. The skip field,
    // not physical adjacency, must select this stream. Leave it enabled
    // across quanta and wait for the hardware-style blocked notification.
    Block inter(8192,256);auto ip=static_cast<BYTE*>(inter.p);std::memset(ip,0xcc,8192);
    for(unsigned p=0;p<2;++p){std::memcpy(ip+p*4096,data.data()+p*2048,2048);ip[p*4096+3]=1;}
    u32 other=4;auto b=ctx[other].w;
    auto setup=[&](u32* v,u32 packets,u32 addr,u32 budget){
        std::fill_n(v,16,0u);v[0]=(cap<<22)|(1<<20)|packets;v[1]=(1u<<31)|((channels-1)<<29)|(3<<27)|(budget<<20);
        v[2]=32;v[5]=addr;v[7]=out.a;v[8]=work.a;v[9]=1u<<30;
    };
    setup(b,4,inter.a,1);b[9]=1u<<31;u32 irq=8;command(2,&irq);
    std::array<u32,16> bitmap{};bitmap[0]=1u<<other;command(6,bitmap.data());
    auto until=GetTickCount64()+2000;while(!hal->available() && GetTickCount64()<until)Sleep(1);
    Message blocked{};require(hal->pop(&blocked) && blocked.type==8 && blocked.detail==other,"autonomous ring completion");
    command(8,bitmap.data());command(3,&irq);
    unsigned filled=(cap/channels)*channels;
    require(b[0]>>27==filled%cap && std::memcmp(out.p,result.data(),filled*256)==0,"autonomous PCM");
    // Continue in single-step mode through the split into the second real packet.
    b[9]=(1u<<30)|(b[0]>>27);b[1]|=1u<<31;
    size_t checked=128*filled;
    while(checked<result.size() && (b[0]&(1u<<20))){
        u32 rd=b[0]>>27;command(5,&other);
        require(!(b[3]>>31),"interleaved stream parser");
        u32 blocks=((b[0]>>27)+cap-rd)%cap;
        for(u32 j=0;j<blocks;++j){
            require(checked+128<=result.size() && !std::memcmp(static_cast<BYTE*>(out.p)+256*((rd+j)%cap),
                    result.data()+checked,256),"interleaved PCM");checked+=128;
        }
        if(!blocks)break;
        b[9]=(1u<<30)|(b[0]>>27);
    }
    require(checked>size_t(data[0]>>2)*512*channels,"packet-skip path not reached");
    if((data[0]>>2)>=5){
        // Loop from transform frame 3 back to frame 1 once. The first
        // transform after the jump rebuilds overlap; frame 2 must match.
        unsigned pos=32+(((data[0]&3)<<13)|(data[1]<<5)|(data[2]>>3));u32 offsets[5]{};
        for(auto& off:offsets){
            off=pos;require(pos+15<16384,"short first fixture packet");
            u32 i=pos/8;pos+=(((u32(data[i])<<16)|(u32(data[i+1])<<8)|data[i+2])>>(9-pos%8))&32767;
        }
        u32 looping=2;auto l=ctx[looping].w;setup(l,4,inter.a,1);
        l[0]|=1<<12;l[1]|=(3<<12)|(1<<17);l[3]=offsets[1];l[4]=offsets[3];
        for(unsigned i=0;i<23;++i){
            u32 rd=l[0]>>27;command(5,&looping);
            require(((l[0]>>27)+cap-rd)%cap==channels && !(l[3]>>31),"loop output");
            if(i==15)require((l[2]&0x3ffffff)==offsets[1] && !((l[0]>>12)&255),"finite loop jump");
            if(i>=19)for(u32 j=0;j<channels;++j)
                require(!std::memcmp(static_cast<BYTE*>(out.p)+256*((rd+j)%cap),
                        result.data()+((8+i-19)*channels+j)*128,256),"loop PCM after overlap");
            l[9]=(1u<<30)|(l[0]>>27);
        }
        require((l[2]&0x3ffffff)==offsets[3],"loop resume");
    }
    // A corrupt packet and an out-of-range ring must report errors, without
    // altering PCM or claiming to have consumed an output block.
    u32 bad=1;b=ctx[bad].w;setup(b,1,in0.a,1);
    std::memset(in0.p,0,2048);command(5,&bad);require(b[3]>>31 && !(b[0]>>27),"bad packet status");
    b[7]=out.a+256;command(5,&bad);require(b[3]>>31 && !(b[0]>>27),"ring bounds");
    check(hal->disconnect(),"disconnect");hal->release();check(AcpHalReleaseShapeContexts(),"release tables");
    std::printf("SHAPE: %u tagged command completions; heap, bounds, and parser-error checks passed\n",msgs);
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"SHAPE check: %s\n",e.what());return 1;}
