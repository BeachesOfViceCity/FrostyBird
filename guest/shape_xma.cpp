#include "shape_xma.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>

namespace shape {
namespace {
constexpr u32 packet_bits=2048*8,offset_mask=(1u<<26)-1,valid_out=1u<<31;
constexpr unsigned rates[]={24000,32000,44100,48000};
constexpr unsigned corrupt=32;
void append(unsigned char* dst,u32 at,const unsigned char* src,u32 bit,u32 n) {
    while(n){
        // Each slice stays inside one packet, including the last byte.
        unsigned width=std::min({8-(at&7),8-(bit&7),n});
        unsigned mask=(1u<<width)-1,value=(src[bit/8]>>(8-(bit&7)-width))&mask;
        unsigned shift=8-(at&7)-width;
        dst[at/8]=static_cast<unsigned char>((dst[at/8]&~(mask<<shift))|(value<<shift));
        at+=width;bit+=width;n-=width;
    }
}
}
void Xma::clear(){
    avcodec_free_context(&decoder);av_frame_free(&frame);av_packet_free(&packet);
    have=length=sub=end_sub=skip=0;loop=tracking=false;
}
Xma::~Xma(){clear();}
bool Xma::transform(unsigned channels,unsigned rate){
    if(!decoder){
        auto codec=avcodec_find_decoder_by_name("xma2frame");
        if(!codec || !(decoder=avcodec_alloc_context3(codec)))return false;
        decoder->sample_rate=int(rate);av_channel_layout_default(&decoder->ch_layout,int(channels));
        decoder->extradata_size=34;
        decoder->extradata=static_cast<uint8_t*>(av_mallocz(34+AV_INPUT_BUFFER_PADDING_SIZE));
        if(!decoder->extradata)return false;
        decoder->extradata[0]=1;
        if(avcodec_open2(decoder,codec,nullptr)<0 || !(frame=av_frame_alloc()) || !(packet=av_packet_alloc()))return false;
    }
    av_packet_unref(packet);av_frame_unref(frame);
    if(av_new_packet(packet,int((length+7)/8))<0)return false;
    std::memcpy(packet->data,bits.data(),packet->size);
    if(avcodec_send_packet(decoder,packet)<0 || avcodec_receive_frame(decoder,frame)<0 ||
       frame->nb_samples!=512 || frame->format!=AV_SAMPLE_FMT_FLTP || unsigned(frame->ch_layout.nb_channels)!=channels)return false;
    for(unsigned c=0;c<channels;++c){
        auto in=reinterpret_cast<const float*>(frame->extended_data[c]);
        for(unsigned i=0;i<512;++i){
            if(!std::isfinite(in[i]))return false;
            pcm[i*channels+c]=short(std::lrint(std::clamp(in[i]*32768.f,-32768.f,32767.f)));
        }
    }
    return true;
}
void Xma::save_position(Context& ctx,Cursor p){
    auto w=ctx.w;w[0]&=~(p.consumed<<20);w[2]=(w[2]&~offset_mask)|(p.bit&offset_mask);
    w[4]=(w[4]&~valid_out)|(p.slot<<31);
}
unsigned Xma::pull(Context& ctx){
    auto w=ctx.w;Cursor p{w[2]&offset_mask,w[4]>>31,0};
    u32 valid=(w[0]>>20)&3;
    auto packets=[&]{return w[p.slot]&0xfff;};
    auto available=[&]{return (valid&~p.consumed)&(1u<<p.slot);};
    auto normalize=[&]{
        while(available() && packets() && p.bit/packet_bits>=packets()){
            p.bit-=packets()*packet_bits;p.consumed|=1u<<p.slot;p.slot^=1;
        }
    };
    auto advance=[&](const BYTE* head,u32 index){
        p.bit=(index+1+head[3])*packet_bits+32;normalize();
    };
    // Each iteration either consumes bits, skips a packet with no frame
    // starts, or releases an input slot. No search outside the advertised ranges.
    for(;;){
        normalize();
        if(!available()){
            save_position(ctx,p);
            return have?8:4;
        }
        auto n=packets();if(!n)return corrupt;
        u32 index=p.bit/packet_bits,rel=p.bit%packet_bits;
        auto data=static_cast<const BYTE*>(map(w[5+p.slot],size_t(n)*2048));
        if(!data)return corrupt;
        auto head=data+size_t(index)*2048;
        if((head[2]&7)!=1)return corrupt;
        w[4]=(w[4]&~(31u<<26))|((head[2]&7u)<<26);
        u32 first=32+(((head[0]&3)<<13)|(head[1]<<5)|(head[2]>>3));
        if(!have){
            if(!(head[0]>>2) || first>=packet_bits){advance(head,index);continue;}
            rel=std::max(rel,first);p.bit=index*packet_bits+rel;
            begin_bit=p.bit;begin_slot=p.slot;
        }else if(rel<32){p.bit+=32-rel;rel=32;}
        u32 count=std::min((length?length:15)-have,packet_bits-rel);
        append(bits.data(),have,head,rel,count);have+=count;p.bit+=count;
        bool boundary=rel+count==packet_bits;
        if(boundary)advance(head,index);
        if(!length && have==15){
            length=(u32(bits[0])<<7)|(bits[1]>>1);
            if(length==0x7fff){
                have=length=0;if(!boundary)advance(head,index);continue;
            }
            if(length<=16)return corrupt;
        }
        if(!length || have!=length)continue;
        unsigned channels=1+((w[1]>>29)&1),rate=rates[(w[1]>>27)&3];
        if(!transform(channels,rate))return corrupt;
        // The trailer says whether another frame *starts in the packet
        // containing this frame's start*. A split frame's zero trailer
        // precedes the first new frame in the continuation packet.
        if(!boundary && (!(head[0]>>2) || rel+count>first) &&
           !(bits[(length-1)/8]&(0x80>>((length-1)&7))))advance(head,index);
        loop=((w[0]>>12)&255) && begin_bit==(w[4]&offset_mask);
        next=p;sub=std::min(skip,4u);skip-=sub;end_sub=loop?1+((w[1]>>12)&3):4;
        have=length=0;
        if(sub==4 && !loop){save_position(ctx,p);valid=(w[0]>>20)&3;p.consumed=0;continue;}
        if(sub>=end_sub)return corrupt;
        return 0;
    }
}
unsigned Xma::run(Context& ctx){
    auto w=ctx.w;u32 slot=w[4]>>31,fmt=(w[1]>>27)&7;
    if(tracking && (expected_bit!=(w[2]&offset_mask) || expected_write!=w[0]>>27 ||
       expected_slot!=slot || (expected_input && expected_input!=w[5+slot]) ||
       expected_output!=w[7] || expected_work!=w[8] || format!=fmt))clear();
    w[2]&=offset_mask;w[3]&=offset_mask;
    unsigned channels=1+(fmt>>2),cap=(w[0]>>22)&31,wr=w[0]>>27,rd=w[9]&31;
    unsigned budget=std::max(1u,(w[1]>>20)&15),padding=(w[1]>>24)&7;
    auto ring=static_cast<BYTE*>(map(w[7],cap*256));
    unsigned emitted=0,status=0;
    if(!(w[1]&valid_out))status=1;
    else if(!ring || !cap || wr>=cap || rd>=cap)status=corrupt;
    else {
        unsigned space=rd>wr?rd-wr:cap-wr+rd;
        while(emitted<budget){
            if(space<channels+padding){status=2;break;}
            if(sub==end_sub && (status=pull(ctx)))break;
            const auto* src=reinterpret_cast<const BYTE*>(pcm.data()+sub*128*channels);
            for(unsigned i=0;i<channels;++i){
                std::memcpy(ring+wr*256,src+i*256,256);wr=(wr+1)%cap;
            }
            space-=channels;++emitted;
            if(++sub==end_sub){
                if(loop){
                    u32 repeat=(w[0]>>12)&255;if(repeat!=255)w[0]-=1u<<12;
                    next={w[3]&offset_mask,begin_slot,0};skip=(w[1]>>17)&7;
                }
                save_position(ctx,next);
            }
            if(!space){w[1]&=~valid_out;break;}
        }
        w[0]=(w[0]&0x07ffffff)|(wr<<27);
    }
    if(status==corrupt){w[3]|=0x80000000u|(1u<<26);clear();}
    else if(status)w[2]|=0x80000000u|(status<<26);
    expected_slot=w[4]>>31;expected_bit=w[2]&offset_mask;expected_write=w[0]>>27;
    expected_input=(w[0]&(1u<<(20+expected_slot)))?w[5+expected_slot]:0;
    expected_output=w[7];expected_work=w[8];format=fmt;tracking=true;
    return emitted;
}
}
