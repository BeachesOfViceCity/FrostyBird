#include "rt_collection.hpp"
#include "shader.hpp"
#include <algorithm>
#include <array>

namespace frosty {
u64 rt_key(Bytes b){
    u64 key=0xcbf29ce484222325ull;
    for(auto v:b.data)key=key*0x100000001b3ull^v;
    return key;
}
Bytes rt_record_blob(Bytes record){
    // Same range the retail RHICreateRayTracingShader passes to
    // DeserializeStateObjectX: after SPTR, before the UE optional data.
    if(record.size()<8)throw Error("short RT shader record");
    auto optional=record.at<u32>(record.size()-4);
    if(optional<4 || optional>record.size()-4)throw Error("invalid RT optional data size");
    auto body=record.slice(0,record.size()-optional);
    constexpr std::string_view tag="SPTR";
    auto it=std::search(body.data.begin(),body.data.end(),tag.begin(),tag.end());
    if(it==body.data.end())throw Unsupported("no serialized RT collection");
    return body.tail(it-body.data.begin()+4);
}
RtCollection rt_collection(Bytes b){
    if(b.size()<20 || b.at<u32>(0)!=2)throw Unsupported("unsupported XS RT collection version");
    RtCollection out{b,{}};
    constexpr std::array<u8,24> prefix{0,0,0x80,0xd5,0,1,0xfe,3,0,0,0,0,0,0,0x80,0xd5,0,1,0xfe,3,0,0,0,0};
    size_t end=0,last_valid=0;
    while(end<b.size()){
        auto t=b.tail(end);
        auto it=std::search(t.data.begin(),t.data.end(),prefix.begin(),prefix.end());
        if(it==t.data.end())break;
        size_t pos=end+size_t(it-t.data.begin());end=pos+prefix.size();
        if(pos<4)continue;
        auto size=b.at<u32>(pos-4);
        if(size<28 || size%4 || size>b.size()-end)continue;
        auto code=b.slice(pos,size);auto rest=b.tail(pos+size);
        if(rest.size()<4)continue;
        auto ms=rest.at<u32>(0);
        if(ms<64 || ms>rest.size()-4)continue;
        auto m=rest.slice(4,ms);
        if(m.at<u32>(0)!=0x403 || m.at<u32>(4)!=20 || m.at<u32>(8)!=size/4 || m.at<u32>(12)!=ms-40)continue;
        auto count=m.at<u32>(56);
        if(!count || count>(ms-64)/24)throw Error("invalid RT symbol count");
        auto table=m.tail(64);
        Bytes root{};
        for(size_t q=last_valid;q+44<=pos;++q)if(b.tag(q,"DXBC")){
            auto d=b.tail(q);auto len=d.at<u32>(24);
            if(len>=44 && len<=pos-q && d.at<u32>(28)==1 && d.tag(d.at<u32>(32),"RTS0")){
                auto off=d.at<u32>(32);
                if(off<=len-8 && d.at<u32>(off+4)==len-off-8)root=d.slice(off+8,len-off-8);
            }
        }
        RtModule module{code,m,root,{},{},0};
        // v2 stores up to three optional traversal libraries ahead of the
        // public module. Each inner header and its export records precede
        // the aligned root/ISA/metadata blobs (xgs12_10's serializer).
        size_t header=last_valid;
        if(out.modules.empty()){
            u32 extra=b.at<u32>(20);
            auto count_at=24ull+extra;
            if(extra!=4 || b.at<u32>(count_at)!=3)throw Unsupported("unknown outer RT configuration");
            header=count_at+4;
        }
        if(out.modules.size()<3){
            if(b.at<u32>(header)!=1)throw Unsupported("unknown traversal module presence");
            header+=4;
        }
        Cursor c{b.slice(header,pos-header)};
        if(c.word()!=2)throw Unsupported("unknown inner RT version");
        c.take(12);module.stack=c.word();auto nx=c.word();
        if(!nx || nx>4096)throw Error("invalid ray export count");
        auto name=[&](){
            auto s=c.array(1);std::string n;
            if(s.size()%2 || s.size()>8192)throw Error("invalid ray export length");
            for(size_t k=0;k<s.size();k+=2){
                auto ch=s.at<std::uint16_t>(k);if(ch<32 || ch>126)throw Unsupported("non-ASCII RT export");
                n+=char(ch);
            }
            return n;
        };
        for(u32 j=0;j<nx;++j){
            RtExport x{};x.name=name();x.frame=c.word();x.stage_frame=c.word();x.type=c.word();
            auto flags=c.take(3);
            for(unsigned k=0;k<3;++k){auto b=flags.at<u8>(k);if(b>1)throw Error("invalid RT export flag");x.flags|=b<<k;}
            for(auto& s:x.symbols)s=name();
            module.exports.push_back(std::move(x));
        }
        for(u32 j=0;j<count;++j){
            auto row=table.tail(24ull*j);auto name=row.at<u64>(0);auto pc=row.at<u32>(8);
            if(name<u64(count)*24 || name>=table.size() || (pc&255) || pc>size-28)
                throw Error("invalid XS RT function table");
            auto s=table.tail(name);auto nul=std::ranges::find(s.data,0);
            if(nul==s.data.end() || nul==s.data.begin() || nul-s.data.begin()>4096)throw Error("invalid RT symbol");
            module.entries.push_back({std::string(s.data.begin(),nul),pc});
        }
        out.modules.push_back(std::move(module));end=last_valid=pos+size+4+ms;
    }
    if(out.modules.empty() || last_valid!=b.size())throw Unsupported("unrecognized XS RT machine module framing");
    return out;
}
}
