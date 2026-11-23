#include "isa.hpp"
#include "rdna2_spec.hpp"
#include <llvm-c/Disassembler.h>
#include <llvm-c/Target.h>
#include <algorithm>
#include <cctype>
#include <format>
#include <deque>
#include <set>
#include <mutex>
#include <optional>

namespace frosty::isa {
namespace {
Bytes chunk(Bytes container, std::string_view name) {
    for (u32 i=0; i<container.at<u32>(28); ++i) {
        auto c=container.tail(container.at<u32>(32ull+4ull*i));
        if (c.tag(0,name)) return c.slice(8,c.at<u32>(4));
    }
    throw Unsupported("missing " + std::string(name));
}
std::vector<Semantic> signature(Bytes c) {
    std::vector<Semantic> out;
    auto rows=c.slice(c.at<u32>(4),32ull*c.at<u32>(0));
    for (unsigned i=0; i<rows.size(); i+=32) {
        auto r=rows.tail(i), name=c.tail(r.at<u32>(4));
        auto end=std::ranges::find(name.data,0);
        if (end==name.data.end()) throw Error("unterminated signature semantic");
        std::string text(name.data.begin(),end);
        if (text.empty() || !std::ranges::all_of(text,[](unsigned char ch){return std::isalnum(ch)||ch=='_';}) ||
            r.at<u32>(0) || r.at<u32>(28)) throw Unsupported("extended signature semantic");
        out.emplace_back(text,r.at<u32>(8),r.at<u32>(12),r.at<u32>(16),r.at<u32>(20),r.at<u8>(24),r.at<u8>(25));
    }
    return out;
}

// LLVM has no gfx1030 mnemonic for a set of horizontal packed-image
// operations in AMD's XML. Preserve the actual base registers and raw fields:
// the component count/return width cannot be inferred from a guessed format.
std::optional<Instruction> packed_image(Bytes b,unsigned pc) {
    auto w=b.at<u32>(0);
    if((w>>26)!=0x3c || b.size()<8)return {};
    auto opcode=[](u32 h){return ((h>>18)&127u)|((h&1u)<<7);};
    const OpcodeSpec* op=nullptr;
    for(const auto& x:catalog)if(x.family=="MIMG" && opcode(x.specimen[0])==opcode(w)){
        if(x.name.ends_with("_by2")||x.name.ends_with("_by4")||x.name.ends_with("_pck2")||
           x.name.ends_with("_pck4")||x.name=="image_gather4h_pck"||x.name=="image_gather8h_pck")op=&x;
        break;
    }
    if(!op)return {};
    auto w1=b.at<u32>(4),n=(w>>1)&3u;
    if(b.size()<8+4*n)throw Unsupported("truncated image NSA operands");
    Instruction i{pc,8+4*n,std::string(op->name),{},{}};
    i.field_decode=true;
    i.args={std::format("vdata=v{}",(w1>>8)&255u),std::format("vaddr=v{}",w1&255u),
            std::format("srsrc=s{}",4*((w1>>16)&31u)),std::format("ssamp=s{}",4*((w1>>21)&31u))};
    i.modifiers=std::format("dim:{} dmask:0x{:x} encoding:{:08x}:{:08x}",(w>>3)&7,(w>>8)&15,w,w1);
    for(unsigned j=0;j<n;++j)i.modifiers+=std::format(":{:08x}",b.at<u32>(8+4*j));
    return i;
}
class Decoder {
    LLVMDisasmContextRef rdna2{}, rdna1{};
public:
    explicit Decoder(unsigned wave) {
        static std::once_flag initialized;
        std::call_once(initialized,[]{
            LLVMInitializeAMDGPUTargetInfo();
            LLVMInitializeAMDGPUTargetMC();
            LLVMInitializeAMDGPUDisassembler();
        });
        auto make=[&](const char* cpu) { return LLVMCreateDisasmCPUFeatures("amdgcn-amd-amdhsa",cpu,
            wave==64?"+wavefrontsize64,-wavefrontsize32":wave==32?"+wavefrontsize32,-wavefrontsize64":"",
            nullptr,0,nullptr,nullptr); };
        rdna2=make("gfx1030"); rdna1=make("gfx1010");
        if (!rdna2 || !rdna1) throw Error("LLVM lacks the AMDGPU disassembler");
    }
    ~Decoder() { LLVMDisasmDispose(rdna2); LLVMDisasmDispose(rdna1); }
    Instruction next(Bytes bytes,unsigned pc) const {
        auto b=bytes.tail(pc);
        char output[1024]{};
        const u32 w=b.at<u32>(0);
        auto read=[&](LLVMDisasmContextRef ctx) {
            return LLVMDisasmInstruction(ctx,const_cast<u8*>(b.data.data()),b.size(),pc,output,sizeof(output));
        };
        auto n=read(rdna2);
        // The XS ISA retains the earlier VOP3 MAD_F32 encoding. Do not retry
        // arbitrary unknown instructions on another architecture or lose sync.
        // Also present in shipped material shaders: VOP2 MAC/MADAK_F32.
        if (!n && ((w>>16)==0xd541 || (w>>16)==0xd51f || (w>>25)==0x1f || (w>>25)==0x21)) n=read(rdna1);
        if(!n){if(auto i=packed_image(b,pc))return *i;throw Unsupported(std::format("unknown encoding 0x{:08x}",w));}
        std::string_view s(output);
        auto first=s.find_first_not_of(" \t");
        if(first==s.npos) throw Unsupported("empty disassembly");
        s.remove_prefix(first);
        auto space=s.find_first_of(" \t");
        Instruction ins{pc,unsigned(n),std::string(s.substr(0,space)),{},{}};
        if (ins.op.ends_with("_e32") || ins.op.ends_with("_e64")) ins.op.resize(ins.op.size()-4);
        if (space==s.npos) return ins;
        s.remove_prefix(space+1);
        // EXP prints the target followed by a space, unlike ordinary operands.
        if (ins.op=="exp") {
            auto end=s.find(' ');
            if (end==s.npos) throw Unsupported("unknown export operands");
            ins.args.emplace_back(s.substr(0,end)); s.remove_prefix(end+1);
        }
        for (;;) {
            auto start=s.find_first_not_of(" \t");
            if (start==s.npos) break;
            s.remove_prefix(start);
            auto end=s.find_first_of(", \t");
            if(s.starts_with('[')) { // MIMG non-sequential address VGPR list
                auto close=s.find(']');
                if(close==s.npos) throw Unsupported("invalid NSA operands");
                end=close+1;
                if(end==s.size()) end=s.npos;
            }
            ins.args.emplace_back(s.substr(0,end));
            if (end==s.npos) break;
            if (s[end]!=',') { ins.modifiers=s.substr(end+1); break; }
            s.remove_prefix(end+1);
        }
        return ins;
    }
};
Bytes stage_descriptor(const Shader& s,Bytes body) {
    if (!s.bitcode.tag(0,"NOBC") || (s.stage!=vertex && s.stage!=pixel && s.stage!=geometry && s.stage!=compute))
        throw Unsupported("unsupported machine shader stage");
    auto slot=body.at<u64>(s.stage==vertex?0x28:s.stage==pixel?0x30:s.stage==geometry?0x20:0x60);
    if (slot>=body.size()) throw Unsupported("alternate FSSM hardware stage");
    return body.tail(slot);
}
Bytes instructions(const Shader& s,Bytes allocation) {
    auto hash=chunk(s.dxbc,"HASH").slice(4,16);
    for (unsigned i=0;i<2;++i) {
        unsigned h=hash.at<std::uint16_t>(8*i+2);
        if (allocation.at<u32>(12*i)!=(0xd5800000|hash.at<std::uint16_t>(8*i)) ||
            allocation.at<u32>(12*i+4)!=(0x03fe0100|((h&0xff00)<<1)|(h&255)) ||
            allocation.at<u32>(12*i+8)!=hash.at<u32>(8*i+4))
            throw Unsupported("unknown machine program preamble");
    }
    auto code=allocation.tail(24);
    if (code.size()%4 || code.size()>1024*1024) throw Unsupported("unsupported ISA allocation");
    return code;
}
}

Program interface(const Shader& s,Buffer& object) {
    auto body=Bytes{object}.tail(16);
    auto d=stage_descriptor(s,body);
    auto allocation=body.slice(d.at<u64>(0),d.at<u64>(8));
    Program p{};p.stage=Stage(s.stage);p.code=instructions(s,allocation);
    auto psv=chunk(s.dxbc,"PSV0");
    auto info=psv.slice(4,psv.at<u32>(0));
    if (info.size()<48 || info.at<u8>(24)!=s.stage) throw Unsupported("unknown PSV runtime info");
    auto bindings=psv.tail(4+info.size());
    auto nr=bindings.at<u32>(0), nx=d.at<u32>(0x3c);
    if (nx>nr || (nr && bindings.at<u32>(4)!=24)) throw Unsupported("alternate resource binding ABI");
    if(nr)bindings.slice(8,24ull*nr);
    // PSV describes the source-level range declarations. The final machine
    // program can have fewer physical SRDs (e.g. an unused cbuffer). Only
    // introduce bindings with a matching Xbox table entry; SMEM still has
    // to resolve each reachable descriptor fetch to its exact offset/width.
    auto rows=nx?body.slice(d.at<u64>(0x30),20ull*nx):Bytes{};
    std::set<unsigned> matched;
    for (unsigned i=0;i<nx;++i) {
        auto x=rows.tail(i*20);
        unsigned cls=x.at<u8>(0), reg=x.at<u32>(8);
        bool found=false;
        for (unsigned j=0;j<nr;++j) {
            auto r=bindings.tail(8+24*j);
            unsigned type=r.at<u32>(0), kind=r.at<u32>(16);
            unsigned group=type==1?1:type==2?2:type>=3 && type<=5?0:type>=6 && type<=9?3:4;
            if (cls!=group || reg!=r.at<u32>(8)) continue;
            if (r.at<u32>(4) || r.at<u32>(12)!=reg || r.at<u32>(20) || x.at<u8>(1)!=3 ||
                x.at<u32>(4)>0xffff || reg>127) throw Unsupported("non-singleton resource or alternate descriptor table");
            if(!matched.insert(j).second)throw Unsupported("duplicate hardware binding for PSV resource");
            p.resources.emplace_back(x.at<u8>(17),4*x.at<u32>(4),x.at<u8>(16),reg,type,kind,x.at<u32>(12));
            found=true; break;
        }
        if (!found) throw Unsupported("PSV/Xbox resource binding mismatch");
    }
    if (s.stage==compute) {
        p.cs_rsrc2=d.at<u32>(0x78);
        p.wave=(d.at<u32>(0x14)&32)?32:64;
        p.lds_bytes=d.at<u32>(0x4c);
        for (unsigned i=0;i<3;++i) {
            auto n=info.at<u32>(36+4*i);
            if (!n || n>1024 || n!=d.at<u32>(0x7c+4*i)) throw Unsupported("unknown compute group dimensions");
            p.threads[i]=n;
        }
    } else {
        p.inputs=signature(chunk(s.dxbc,"ISG1"));
        p.outputs=signature(chunk(s.dxbc,"OSG1"));
        // PSV carries interpolation qualifiers; ISG1/OSG1 alone do not.
        Cursor sig{bindings.tail(nr?8ull+24ull*nr:4)};
        auto strings=sig.array(1),indices=sig.array(4);
        if((info.at<u8>(28) || info.at<u8>(29)) && sig.word()!=16)throw Unsupported("unknown PSV element size");
        for (auto* list:{&p.inputs,&p.outputs}) {
            std::vector<bool> seen(list->size());
            // PSV groups semantic arrays into one record with N rows. ISG1
            // and OSG1 enumerate those rows separately (often TEXCOORDn).
            for(unsigned i=0;i<info.at<u8>(list==&p.inputs?28:29);++i) {
                auto row=sig.take(16);
                unsigned cols=row.at<u8>(10)&15,start=(row.at<u8>(10)>>4)&3;
                unsigned n=row.at<u8>(8),base=row.at<u8>(9);
                if(!n || !cols || start+cols>4)
                    throw Unsupported("extended PSV signature element");
                auto mask=((1u<<cols)-1)<<start;
                auto nm=strings.tail(row.at<u32>(0));auto z=std::ranges::find(nm.data,0);
                if(z==nm.data.end())throw Error("unterminated PSV semantic");
                std::string_view name(reinterpret_cast<const char*>(nm.data.data()),z-nm.data.begin());
                for(unsigned k=0;k<n;++k) {
                    auto reg=row.at<u8>(10)&64?base+k:~0u;
                    auto idx=indices.at<u32>((u64(row.at<u32>(4))+k)*4);
                    auto it=std::ranges::find_if(*list,[&](const Semantic& sem) {
                        return !seen[&sem-list->data()] && sem.reg==reg && sem.mask==mask && sem.type==row.at<u8>(12) &&
                            (name.empty() || (sem.name==name && sem.index==idx));
                    });
                    if(it==list->end())throw Unsupported("signature packing mismatch");
                    seen[it-list->begin()]=true;
                    it->interpolation=row.at<u8>(13);
                }
            }
            if(std::ranges::find(seen,false)!=seen.end())throw Unsupported("missing PSV rows");
        }
        if(s.stage==geometry) {
            if(info.at<u32>(0)!=3 || info.at<u32>(4)!=5 || info.at<u32>(8)!=1)
                throw Unsupported("non-triangle or multi-stream GS");
            p.max_vertices=info.at<std::uint16_t>(26);p.output_vectors=info.at<u8>(32);
            if(!p.max_vertices || p.max_vertices>1024 || !p.output_vectors)throw Unsupported("invalid geometry output limits");
        } else if (s.stage==vertex) {
            p.wave=(d.at<u32>(0x14)&32)?32:64;
            auto count=d.at<u32>(0x118), params=d.at<u32>(0x1dc);
            if (count>32 || params>32) throw Unsupported("extended vertex linkage");
            for (unsigned i=0;i<count;++i) {
                auto r=d.tail(0x78+5*i);
                if (r.at<u8>(0)!=6 || r.at<u8>(2)!=(1u<<(1+(r.at<u8>(4)&3)))-1 || r.at<u8>(4)>3)
                    throw Unsupported("non-contiguous vertex fetch");
                p.fetch.emplace_back(r.at<u8>(1),r.at<u8>(2),r.at<u8>(3));
            }
            for (unsigned i=0;i<params;++i) {
                auto r=d.tail(0x11c+6*i);
                if(r.at<u8>(0)==7 && !r.at<u8>(3) && !r.at<std::uint16_t>(4) &&
                   std::ranges::any_of(p.outputs,[&](const Semantic& x){return x.system==2 && x.reg==r.at<u8>(1);})){
                    p.clip_parameters.emplace(r.at<u8>(2),r.at<u8>(1));continue;
                }
                if(r.at<u8>(0)==17 && !r.at<u8>(3) && !r.at<std::uint16_t>(4) &&
                   std::ranges::any_of(p.outputs,[&](const Semantic& x){return x.system==4 && x.reg==r.at<u8>(1);})){
                    p.layer_parameters.emplace(r.at<u8>(2),r.at<u8>(1));continue;
                }
                if (r.at<u8>(0)!=6 || r.at<u8>(3) || r.at<std::uint16_t>(4) ||
                    !p.parameters.emplace(r.at<u8>(2),r.at<u8>(1)).second)
                    throw Unsupported("extended parameter export");
            }
            if(std::ranges::none_of(p.outputs,[](const Semantic& s){return s.system==1;})){
                unsigned reg=0;while(std::ranges::any_of(p.outputs,[&](const Semantic& s){return s.reg==reg;}))++reg;
                // Some VS signatures omit the fixed-function position that
                // the machine code still exports to POS0. Forward those words.
                p.outputs.push_back({"SV_Position",0,1,3,reg,15,0,0});
            }
            // For an output, DXBC ReadWriteMask marks *never written*
            // components. Some retail signatures retain entire dead semantics.
            for(const auto& s:p.outputs)if(!s.system && (s.mask&s.used)==s.mask)p.dead_parameter_regs.push_back(s.reg);
            std::erase_if(p.outputs,[](const Semantic& s){return !s.system && (s.mask&s.used)==s.mask;});
        } else {
            unsigned count=d.at<u32>(0x178);
            if (count>32) throw Unsupported("extended pixel linkage");
             p.ps_input=d.at<u32>(0x188);
             // DB_SHADER_CONTROL follows SPI_PS_INPUT_ADDR in this FSSM.
             // Early-Z eligibility (Z_ORDER = 1) alone does not authorize
             // suppressing an occluded pixel's UAV writes. DEPTH_BEFORE_SHADER
             // is the explicit ordering request. Preserve KILL and EXEC_ON_NOOP
             // for the color-only forms, without conflating depth exports or
             // EXEC_ON_HIER_FAIL with this contract.
             const auto db=d.at<u32>(0x18c);
             p.depth_before_pixel=(db&~0x440u)==0x1010u &&
                     std::ranges::all_of(p.outputs,[](const Semantic& s){return s.system==64;});
             p.wave=(d.at<u32>(0x14)&32)?32:64;
             p.flexible_pixel_wave=info.at<u32>(16)==0 && info.at<u32>(20)==~0u;
            // Perspective/linear sample, center and centroid each occupy
            // an I,J pair. Pull-model and line-stipple have a different ABI.
            if (p.ps_input&0x88u)
                throw Unsupported("alternate pixel barycentric ABI");
            for (unsigned i=0;i<count;++i) {
                auto r=d.tail(0x78+8*i);
                if(r.at<u8>(0)==17) { // indexed rasterizer layer arrives in ancillary bits, not a parameter barycentric
                    auto sem=std::ranges::find(p.inputs,unsigned(r.at<u8>(1)),&Semantic::reg);
                    if(sem==p.inputs.end() || sem->system!=4 || r.at<u8>(7)!=1 || !(p.ps_input&(1u<<13)))
                        throw Unsupported("unknown system interpolant");
                    continue;
                }
                if (r.at<u8>(0)!=6 || (r.at<u8>(7)!=2 && r.at<u8>(7)!=1) || r.at<std::uint16_t>(4) ||
                    !p.interpolants.emplace(r.at<u8>(3),r.at<u8>(1)).second)
                    throw Unsupported("extended interpolant");
            }
            // FSSM includes ten export instruction sequences per render target.
            // Use the shipping full-width (SPI_SHADER_32_ABGR) sequence:
            // desktop D3D performs the final conversion for the bound RTV.
            // Reinterpreting the default packed-FP16 epilog would lose integer
            // bits and even round float MRTs before their format was known.
            for(const auto& out:p.outputs) if(out.system==64) {
                if(out.index>=8 || d.at<u32>(0x19c)!=6) throw Unsupported("unknown pixel export patch table");
                auto table=d.tail(0x1a0+404*out.index);
                u64 offset=4ull*table.at<u32>(0);
                if(offset<24 || offset+24>allocation.size()) throw Unsupported("invalid MRT patch location");
                auto original=allocation.slice(offset,24);
                bool matched=false;
                for(unsigned j=0;j<10;++j) matched|=std::ranges::equal(original.data,table.slice(4+40*j,24).data);
                if(!matched) throw Unsupported("unrecognized live MRT epilog");
                auto full=table.slice(4+40*9,24);
                if ((full.at<u32>(16)&0xffff03f0u)!=(0xf8000000u|(out.index<<4))) throw Unsupported("unknown full-width MRT export");
                std::copy(full.data.begin(),full.data.end(),object.data()+(original.data.data()-object.data()));
            }
        }
    }
    return p;
}

Bytes machine_code(const Shader& s,const Buffer& object) {
    auto body=Bytes{object}.tail(16);
    // The mesh-stage FSSM directory entry is also an offset/size-backed ISA
    // allocation. Count/disassemble it independently of desktop mesh ABI support.
    auto d=s.stage==13 && s.bitcode.tag(0,"NOBC")?body.tail(body.at<u64>(0x48)):stage_descriptor(s,body);
    return instructions(s,body.slice(d.at<u64>(0),d.at<u64>(8)));
}
unsigned machine_wave(const Shader& s,const Buffer& object){
    if(s.stage!=compute && s.stage!=pixel && s.stage!=vertex)return 0;
    auto d=stage_descriptor(s,Bytes{object}.tail(16));return d.at<u32>(0x14)&32?32:64;
}
Instruction decode(Bytes code,unsigned pc,unsigned wave) {
    thread_local Decoder standard(0),small(32),large(64);
    auto& decoder=wave==64?large:wave==32?small:standard;
    return decoder.next(code,pc);
}
Graph decode_graph(Bytes code,std::vector<std::string>* gaps,unsigned wave,bool vertex_fetch) {
    Graph g;
    std::deque<unsigned> todo{0};std::set<unsigned> heads{0};
    while(!todo.empty()){
        unsigned pc=todo.front();todo.pop_front();
        try {while(!g.ins.contains(pc)){
            if(pc>=code.size())throw Unsupported("CFG edge outside code allocation");
            Instruction i;
            try {i=decode(code,pc,wave);}catch(const Unsupported& e){throw Unsupported(std::format("code+0x{:x}: {}",pc,e.what()));}
            auto next=pc+i.size;
            auto hi=g.ins.lower_bound(pc);
            if((hi!=g.ins.end() && next>hi->first) || (hi!=g.ins.begin() && std::prev(hi)->first+std::prev(hi)->second.size>pc))
                throw Unsupported("CFG jump into an operand");
            bool branch=i.op=="s_branch" || i.op.starts_with("s_cbranch_"),stop=i.op=="s_endpgm";
            if(i.op=="s_barrier")heads.insert(next); // a resumable wave rendezvous
            if(gaps && i.op.starts_with("s_")){
                if(auto meta=opcode_spec(i.op)){
                    stop|=(meta->flags&8)!=0;
                    if((meta->flags&1) && !branch){
                        bool fetch=vertex_fetch && i.op=="s_swappc_b64" && pc==0 && i.args==std::vector<std::string>{"s[4:5]","s[4:5]"};
                        if(!fetch){gaps->push_back(std::format("code+0x{:x}: unresolved control transfer {}",pc,i.op));stop=true;}
                    }
                }
            }
            if(branch) {
                auto word=code.at<u32>(pc);
                auto disp=std::bit_cast<std::int16_t>(std::uint16_t(word));
                auto to=std::int64_t(next)+4ll*disp;
                if(to<0 || u64(to)>=code.size())throw Unsupported("CFG jump outside machine record");
                heads.insert(unsigned(to));todo.push_back(unsigned(to));
                if(i.op!="s_branch"){heads.insert(next);todo.push_back(next);}
            }
            g.ins.emplace(pc,std::move(i));if(branch||stop)break;pc=next;
        }}catch(const Error& e){if(!gaps)throw;gaps->emplace_back(e.what());}
    }
    // The audit keeps every reachable instruction it could decode, including
    // other arms of a branch when one block ends in an unknown encoding.
    if(gaps)return g;
    for(unsigned h:heads) {
        auto& edges=g.edges[h];
        for(unsigned pc=h;;){
            const auto& i=g.ins.at(pc);auto next=pc+i.size;
            if(i.op=="s_endpgm")break;
            if(i.op=="s_branch" || i.op.starts_with("s_cbranch_")){
                auto disp=std::bit_cast<std::int16_t>(std::uint16_t(code.at<u32>(pc)));
                edges.push_back(unsigned(std::int64_t(next)+4ll*disp));
                if(i.op!="s_branch" && next!=edges[0])edges.push_back(next);
                break;
            }
            if(heads.contains(next)){edges.push_back(next);break;}pc=next;
        }
    }
    std::map<unsigned,unsigned> incoming;
    for(auto& [h,edges]:g.edges){incoming.try_emplace(h,0);for(auto e:edges)++incoming[e];}
    for(auto [h,n]:incoming)if(!n)todo.push_back(h);
    while(!todo.empty()){auto h=todo.front();todo.pop_front();g.order.push_back(h);
        for(auto e:g.edges[h])if(!--incoming.at(e))todo.push_back(e);}
    if(g.order.size()!=heads.size())g.order.clear();
    return g;
}
std::string listing(const Shader& s) {
    if (!s.xbch.size()) throw Unsupported("no machine object");
    auto obj=s.machine_object();
    auto code=machine_code(s,obj);
    std::string out;
    for (auto& [pc,i]:decode_graph(code,nullptr,machine_wave(s,obj)).ins) {
        out+=std::format("{:05x}  {}",pc,i.op);
        for (auto& a:i.args) out+=" "+a;
        out+=" "+i.modifiers+"\n";
    }
    return out;
}
} // namespace frosty::isa
