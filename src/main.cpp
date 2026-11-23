#include "archive.hpp"
#include "audit.hpp"
#include "isa.hpp"
#include "rt_collection.hpp"
#include "rt_machine.hpp"
#include <vulkan/vulkan.h>

#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace frosty;

namespace {
class Mapping {
    void* p_;
    size_t size_;
public:
    explicit Mapping(const char* path) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw Error(std::format("{}: {}", path, strerror(errno)));
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            close(fd);
            throw Error("expected a nonempty regular Pak file");
        }
        size_ = st.st_size;
        p_ = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p_ == MAP_FAILED) throw Error("cannot map Pak file");
    }
    Mapping(const Mapping&) = delete;
    ~Mapping() { munmap(p_, size_); }
    Bytes bytes() const { return {{static_cast<const u8*>(p_), size_}}; }
};

void write_file(const fs::path& file, const void* data, size_t size) {
    std::ofstream f;
    f.exceptions(std::ios::badbit | std::ios::failbit);
    f.open(file, std::ios::binary);
    f.write(static_cast<const char*>(data), size);
    f.close();
}

void emit(const Shader& shader,const fs::path& dir,bool debug) {
    auto t=isa::recompile(shader);
    auto stem=std::format("{:016x}.{}",shader.key(),isa::stage_name(t.stage));
    std::vector<Buffer> variants;
    for(unsigned n=0;n<(t.views.size()==1?3u:1u);++n) variants.push_back(isa::compile_dxil(t,std::string(t.views.size(),char('0'+n))));
    fs::create_directories(dir);
    const char* suffixes[]{t.views.empty()?".dxil":".float.dxil",".uint.dxil",".sint.dxil"};
    for(unsigned i=0;i<variants.size();++i)
        write_file(dir/(stem+suffixes[i]),variants[i].data(),variants[i].size());
    if(!t.views.empty())write_file(dir/(stem+".views"),t.views.data(),4*t.views.size());
    if(debug) {
        write_file(dir/(stem+".hlsl"),t.source.data(),t.source.size());
        write_file(dir/std::format("{:016x}.dxbc",shader.key()),shader.dxbc.data.data(),shader.dxbc.size());
        std::cout<<dir/(stem+suffixes[0])<<"\n";
    }
}
void prepare(Bytes pak, const fs::path& dir) {
    fs::create_directories(dir);
    for (auto name : {"Global", "Frosty"}) {
        auto archive = open_library(pak, name);
        unsigned written = 0;
        std::map<unsigned,unsigned> counts;
        for (u32 i = 0; i < archive.size(); ++i) {
            auto entry = archive[i];
            if (entry.frequency != 0 && entry.frequency != 3 && entry.frequency != 4 && entry.frequency != 5) continue;
            try {
                auto record = entry.unpack();
                auto shader = inspect_shader({record});
                emit(shader,dir,false);
                ++counts[shader.stage];
                ++written;
            } catch (const Unsupported&) {
                continue;
            } catch (const std::exception& e) {
                throw Error(std::format("{}[{}]: {}", name, i, e.what()));
            }
        }
        std::cout << std::format("{}: recompiled {}/{} records (VS {}, GS {}, PS {}, CS {})\n",
            name,written,archive.size(),counts[1],counts[2],counts[0],counts[5])<<std::flush;
    }
}

void devices() {
    VkApplicationInfo app{.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pNext=nullptr,
        .pApplicationName="Frosty", .applicationVersion=1, .pEngineName=nullptr,
        .engineVersion=0, .apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance instance{};
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) throw Error("vkCreateInstance failed");
    struct Guard { VkInstance i; ~Guard() { vkDestroyInstance(i, nullptr); } } guard{instance};
    u32 n = 0;
    if (vkEnumeratePhysicalDevices(instance, &n, nullptr) != VK_SUCCESS || !n)
        throw Error("no Vulkan physical devices");
    std::vector<VkPhysicalDevice> gpus(n);
    if (vkEnumeratePhysicalDevices(instance, &n, gpus.data()) != VK_SUCCESS)
        throw Error("vkEnumeratePhysicalDevices failed");
    for (auto gpu : std::span(gpus).first(n)) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpu, &p);
        std::cout << std::format("{} [{:04x}:{:04x}] Vulkan {}.{}.{} | shader route: SPIR-V\n",
            p.deviceName, p.vendorID, p.deviceID, VK_VERSION_MAJOR(p.apiVersion),
            VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    }
}

int usage() {
    std::cerr << "frosty-shader devices\n"
                 "frosty-shader prepare <Frosty-XSX.pak> <override-dir>\n"
                 "frosty-shader cache-inputs <Frosty-XSX.pak> [--selected] (binary stdout)\n"
                 "frosty-shader audit <Frosty-XSX.pak> <report-dir> [--dxil|--require-all] [--jobs N]\n"
                 "frosty-shader catalog <opcode-probes.tsv>\n"
                 "frosty-shader source <original.dxbc> [compute-caps]\n"
                 "frosty-shader test-kernel <isa.bin> <wave> <threads> <lds-bytes> <stride> <output.dxil>\n"
                 "frosty-shader test-pixel-wave|test-pixel-cube|test-pixel-interp <isa.bin> <output.dxil>\n"
                 "frosty-shader inventory <Frosty-XSX.pak> <Global|Frosty>\n"
                 "frosty-shader global-index <Frosty-XSX.pak> <output-dir>\n"
                 "frosty-shader rt-index <pak> <index.tsv>\n"
                 "frosty-shader rt-list <collection.xsrt> [entry.ss_0]\n"
                 "frosty-shader rt-lift <pak> <library> <index> <output-dir> <payload-words> [t1=32,u0=4]\n"
                 "frosty-shader rt-guard <pak> <library> <index> <measurement-dir>\n"
                 "frosty-shader rt-bindings <pak> <library> <index>\n"
                 "frosty-shader rt-lift-checked <pak> <library> <index> <dir> <words> <views> <route> <payload-word> <value>\n"
                 "frosty-shader rt-hit-specialize <collection.xsrt> <dir> <words> <payload-word> <value>\n"
                 "frosty-shader inspect <pak> <library> <index>\n"
                 "frosty-shader lookup <pak> <hex-shader-key> [...]\n"
                 "frosty-shader disasm <pak> <library> <index>\n"
                 "frosty-shader dump <pak> <library> <index> <output-dir>\n"
                 "frosty-shader lift <pak> <library> <index> <output-dir>\n";
    return 2;
}
} // namespace

int main(int argc, char** argv) try {
    if (argc < 2) return usage();
    const std::string_view cmd = argv[1];
    if(cmd=="cache-inputs" && (argc==3 || (argc==4 && std::string_view(argv[3])=="--selected"))){
        // Stream the exact stage containers, not .float.dxil overrides. The
        // guest and offline warmer use the same compiler packet path. The
        // short keys only identify candidates in older trace logs.
        Mapping pak(argv[2]);std::set<std::pair<u32,u64>> wanted,found;
        if(argc==4){
            std::string key;u32 stage;
            while(std::cin>>std::dec>>stage>>key){
                u64 k{};auto [end,ec]=std::from_chars(key.data(),key.data()+key.size(),k,16);
                if(ec!=std::errc{} || end!=key.data()+key.size() || key.size()!=16)throw Error("invalid trace key");
                wanted.emplace(stage,k);
            }
            if(wanted.empty())throw Error("empty stage selection");
        }
        for(auto lib:{"Global","Frosty"}){
            auto archive=open_library(pak.bytes(),lib);
            for(u32 i=0;i<archive.size();++i){
                auto e=archive[i];if(e.frequency!=0 && e.frequency!=3 && e.frequency!=4 && e.frequency!=5)continue;
                auto record=e.unpack();auto s=inspect_shader({record});
                if(!s.xbch.size() || !s.bitcode.tag(0,"NOBC"))continue;
                auto k=s.key();auto id=std::pair{s.stage,k};
                if(argc==4 && !wanted.contains(id))continue;
                if(argc==4)found.insert(id);
                u32 header[]{s.stage,u32(s.dxbc.size())};
                std::cout.write(reinterpret_cast<const char*>(&k),8);
                std::cout.write(reinterpret_cast<const char*>(header),sizeof(header));
                std::cout.write(reinterpret_cast<const char*>(s.dxbc.data.data()),s.dxbc.size());
                if(!std::cout)throw Error("failed to stream original bytecode");
            }
        }
        if(found!=wanted)throw Error(std::format("{} traced stages were not found in this package",wanted.size()-found.size()));
        return 0;
    }
    if (cmd == "devices" && argc == 2) { devices(); return 0; }
    if(cmd=="source" && (argc==3 || argc==4)){
        Mapping input(argv[2]);auto s=inspect_shader(input.bytes());unsigned caps=0;
        if(argc==4){std::string_view v=argv[3];auto [p,ec]=std::from_chars(v.data(),v.end(),caps);
            if(ec!=std::errc{} || p!=v.end() || caps>15 || s.stage!=isa::compute)return usage();}
        std::cout<<isa::recompile(s,caps).source;return !std::cout;
    }
    if(cmd=="catalog" && argc==3){probe_catalog(argv[2]);return 0;}
    if(cmd=="global-index" && argc==4){
        Mapping pak(argv[2]);auto archive=open_library(pak.bytes(),"Global");auto cache=global_shader_cache(pak.bytes());fs::path dir=argv[3];
        fs::create_directories(dir);write_file(dir/"global-cache.bin",cache.data(),cache.size());
        std::ofstream f;f.exceptions(std::ios::failbit|std::ios::badbit);f.open(dir/"global-maps.tsv");
        f<<"section\tmap_hash\tlocal_index\tglobal_index\tkey\n";
        std::map<u32,u64> keys;
        for(u32 m=0;m<archive.map_count();++m){
            std::string hash;for(auto b:archive.map_hash(m).data)hash+=std::format("{:02x}",b);
            auto ids=archive.map_indices(m);
            for(unsigned j=0;j<ids.size()/4;++j){
                auto i=ids.at<u32>(4ull*j);if(!keys.contains(i)){auto raw=archive[i].unpack();keys[i]=inspect_shader({raw}).key();}
                f<<std::format("{}\t{}\t{}\t{}\t{:016x}\n",m,hash,j,i,keys.at(i));
            }
        }
        std::cout<<archive.map_count()<<" global maps, "<<keys.size()<<" shader records\n";return 0;
    }
    if((cmd=="test-pixel-wave" || cmd=="test-pixel-cube" || cmd=="test-pixel-interp") && argc==4){
        Mapping input(argv[2]);isa::Program p{};p.stage=isa::pixel;p.code=input.bytes();p.wave=64;p.ps_input=256;p.flexible_pixel_wave=true;
        bool cube=cmd=="test-pixel-cube",interp=cmd=="test-pixel-interp";
        if(cube||interp){p.ps_input|=512;p.resources={{8,0,8,0,3,cube?9u:2u,4},{10,0,4,0,1,14,0}};}
        if(interp){p.ps_input|=2|(1u<<12);p.inputs.push_back({"TEXCOORD",0,0,3,1,3,3,2});p.interpolants[0]=1;}
        p.inputs.push_back({"SV_Position",0,1,3,0,15,cube||interp?3u:1u,4});p.outputs.push_back({"SV_Target",0,64,3,0,15,0,0});
        if(interp)p.inputs.push_back({"SV_IsFrontFace",0,9,1,2,1,1,1});
        auto t=isa::recompile(p);auto code=isa::compile_dxil(t);write_file(argv[3],code.data(),code.size());return 0;
    }
    if(cmd=="test-kernel" && (argc==8||argc==9)) {
        Mapping input(argv[2]);isa::Program p{};p.stage=isa::compute;p.code=input.bytes();
        auto integer=[](std::string_view s){unsigned v;auto [e,ec]=std::from_chars(s.data(),s.data()+s.size(),v);
            if(ec!=std::errc{} || e!=s.data()+s.size())throw Error("invalid test ABI parameter");
            return v;};
        p.wave=integer(argv[3]);p.threads={integer(argv[4]),1,1};p.lds_bytes=integer(argv[5]);auto stride=integer(argv[6]);
        p.native_compute_wave=std::getenv("FROSTY_TEST_NATIVE_WAVE") && p.threads[0]>=p.wave;
        p.native_half=std::getenv("FROSTY_TEST_NATIVE_HALF")!=nullptr;
        p.native_group_id=p.native_compute_wave && std::getenv("FROSTY_TEST_GROUP_ORDINAL");
        if((p.wave!=32 && p.wave!=64)||!p.threads[0]||p.threads[0]>1024||p.lds_bytes>32768||p.lds_bytes%4||
            !stride||stride>2048||stride%4)throw Error("invalid test ABI");
        // Deliberately separate from the retail stage linker: group X in s8,
        // local X in v0, s6 points to structured u0 and an optional image u1.
        p.cs_rsrc2=8u*2+128;p.resources.push_back({6,0,4,0,8,12,stride});
        p.resources.push_back({6,32,8,1,6,2,4});
        p.resources.push_back({6,64,8,0,3,2,4});
        p.resources.push_back({6,96,4,0,1,14,0});
        p.resources.push_back({6,128,4,0,5,12,4}); // alternative read-only buffer fixture
        auto t=isa::recompile(p);fs::path out=argv[7];
        auto code=isa::compile_dxil(t,argc==9?argv[8]:"");write_file(out,code.data(),code.size());
        if(!t.views.empty()){auto views=out;views.replace_extension(".views");write_file(views,t.views.data(),4*t.views.size());}
        out.replace_extension(".hlsl");write_file(out,t.source.data(),t.source.size());return 0;
    }
    if(cmd=="rt-bindings" && argc==5){
        Mapping f(argv[2]);auto ar=open_library(f.bytes(),argv[3]);auto n=std::stoul(argv[4]);
        if(n>=ar.size() || ar[u32(n)].frequency!=6)throw Error("not a raygen record");
        auto raw=ar[u32(n)].unpack();auto c=rt_collection(rt_record_blob({raw}));auto& m=c.modules.back();
        auto rg=std::ranges::find_if(m.exports,[](auto& e){return e.type==0 && !(e.flags&3);});
        if(rg==m.exports.end())throw Error("no public raygen");
        auto p=ray_program(m,*rg);size_t blocks=0;for(auto& part:p.parts)blocks+=part.blocks.size();
        std::cout<<std::format("{}: {} continuations, {} blocks, {}-word frame, {} stack words\n",rg->name,p.parts.size(),blocks,p.frame_words,p.stack_words);
        constexpr const char* shapes[]{"CBV","Texture2D","Texture3D","RWTexture2D","Buffer","TLAS","buffer words"};
        for(auto [b,u]:p.used)std::cout<<std::format("space{} {} {} {}\n",b.space,b.name(),shapes[int(u.view)],u.words);
        return 0;
    }
    if(cmd=="rt-guard" && argc==6){
        Mapping f(argv[2]);auto a=open_library(f.bytes(),argv[3]);auto n=std::stoul(argv[4]);
        if(n>=a.size() || a[u32(n)].frequency!=6)throw Error("not a ray-generation record");
        auto raw=a[u32(n)].unpack();auto c=rt_collection(rt_record_blob({raw}));
        auto source=rt_guard_source(c);auto dxil=compile_rt_library(source);fs::path dir=argv[5];
        fs::create_directories(dir);write_file(dir/"launch-guard.hlsl",source.data(),source.size());
        write_file(dir/"launch-guard.dxil",dxil.data(),dxil.size());std::cout<<dir/"launch-guard.dxil"<<'\n';return 0;
    }
    if(cmd=="rt-rg-packet" && argc==7){
        Buffer raw{std::istreambuf_iterator<char>(std::cin),{}};auto c=rt_collection({raw});
        auto n=[](const char* v){return u32(std::stoul(v,nullptr,0));};
        auto t=ray_live_source(c,n(argv[2]),{n(argv[3]),n(argv[4]),n(argv[5])},argv[6]);
        auto dxil=compile_rt_library(t.source);u32 header[]{u32(t.bindings.size()),u32(dxil.size())};
        std::cout.write(reinterpret_cast<const char*>(header),sizeof(header));std::cout.write(reinterpret_cast<const char*>(t.bindings.data()),4*t.bindings.size());
        std::cout.write(reinterpret_cast<const char*>(dxil.data()),dxil.size());return 0;
    }
    if((cmd=="rt-hit-packet" || cmd=="rt-miss-packet") && argc==6){
        Buffer raw{std::istreambuf_iterator<char>(std::cin),{}};auto c=rt_collection({raw});auto& m=c.modules.back();
        bool hit=cmd=="rt-hit-packet";
        auto e=std::ranges::find_if(m.exports,[&](auto& e){return e.type==(hit?2u:4u) && !(e.flags&3) && e.symbols[0]==argv[5];});
        if(e==m.exports.end())throw Unsupported("no such original ray export");
        auto source=hit?ray_hit_specialization(m,*e,std::stoul(argv[2]),std::stoul(argv[3]),std::stoul(argv[4])):rt_source(c,7,std::stoul(argv[2]),{});
        auto dxil=compile_rt_library(source);u32 header[]{0,u32(dxil.size())};
        std::cout.write(reinterpret_cast<const char*>(header),sizeof(header));std::cout.write(reinterpret_cast<const char*>(dxil.data()),dxil.size());return 0;
    }
    if(cmd=="rt-hit-specialize" && argc==7){
        Mapping f(argv[2]);auto c=rt_collection(f.bytes());auto& m=c.modules.back();auto words=std::stoul(argv[4]);
        auto word=std::stoul(argv[5]),value=std::stoul(argv[6],nullptr,0);
        auto e=std::ranges::find_if(m.exports,[](auto& e){return e.type==2 && !(e.flags&3);});
        if(e==m.exports.end())throw Unsupported("no original closest-hit export");
        auto source=ray_hit_specialization(m,*e,words,word,value);auto code=compile_rt_library(source);
        fs::path dir=argv[3];fs::create_directories(dir);auto stem=(dir/std::format("{:016x}",rt_key(c.blob))).string();
        write_file(stem+".xsrt",c.blob.data.data(),c.blob.size());write_file(stem+".hit.hlsl",source.data(),source.size());
        write_file(stem+".hit.dxil",code.data(),code.size());std::ofstream abi(stem+".hit.plan");
        abi<<words<<' '<<word<<' '<<value<<' '<<e->symbols[0]<<'\n';return 0;
    }
    if((cmd=="rt-lift" && (argc==7 || argc==8)) || (cmd=="rt-lift-checked" && argc==11)){
        Mapping f(argv[2]);auto a=open_library(f.bytes(),argv[3]);auto n=std::stoul(argv[4]);if(n>=a.size())throw Error("invalid shader index");
        auto entry=a[u32(n)];auto raw=entry.unpack();auto c=rt_collection(rt_record_blob({raw}));
        auto words=std::stoul(argv[6]);if(!words || words>32)throw Error("invalid RT payload width");
        bool checked=cmd=="rt-lift-checked";std::string hlsl;
        if(checked){
            auto& m=c.modules.back();auto rg=std::ranges::find_if(m.exports,[](auto& e){return e.type==0 && !(e.flags&3);});
            if(entry.frequency!=6 || rg==m.exports.end())throw Error("not a ray-generation program");
            auto n=[](const char* s){return u32(std::stoul(s,nullptr,0));};
            hlsl=ray_machine_source(m,*rg,u32(words),argv[7],RayContract{n(argv[8]),n(argv[9]),n(argv[10])});
        }else hlsl=rt_source(c,entry.frequency,u32(words),argc==8?argv[7]:"");
        auto dxil=compile_rt_library(hlsl);
        auto stem=fs::path(argv[5])/std::format("{:016x}",rt_key(c.blob));fs::create_directories(stem.parent_path());
        auto file=stem;file+=checked?".checked.hlsl":".rt.hlsl";write_file(file,hlsl.data(),hlsl.size());
        file=stem;file+=".xsrt";write_file(file,c.blob.data.data(),c.blob.size());
        file=stem;file+=".rt.abi";u32 abi[]{u32(words*4),8};write_file(file,abi,sizeof(abi));
        file=stem;file+=".rt.exports";std::ofstream names(file);
        for(auto& e:c.modules.back().exports)if(!(e.flags&2))names<<(e.flags&1?"group":"shader")<<'\t'<<e.name<<'\n';
        if(!names)throw Error("cannot write ray exports");
        file=stem;file+=checked?".checked.dxil":".rt.dxil";write_file(file,dxil.data(),dxil.size());
        std::cout<<file<<'\n';return 0;
    }
    if(cmd=="rt-list" && (argc==3 || argc==4)){
        Mapping f(argv[2]);auto c=rt_collection(f.bytes());std::cout<<std::format("collection {:016x}, {} modules\n",rt_key(c.blob),c.modules.size());
        bool found=false;
        for(size_t j=0;j<c.modules.size();++j)for(auto& e:c.modules[j].entries){
            if(argc==4 && e.name!=argv[3])continue;
            found=true;std::cout<<std::format("module {} +0x{:x} {}\n",j,e.offset,e.name);
            if(argc==4)std::cout<<rt_listing(c.modules[j],e);
        }
        return found?0:1;
    }
    if(cmd=="rt-index" && argc==4){
        Mapping f(argv[2]);std::ofstream out(argv[3]);if(!out)throw Error("cannot write ray-collection index");
        out<<"library\tindex\tfrequency\tcollection_key\tbytes\tmodules\tentry_count\tdetail\tshader_sha1\n";
        size_t total=0,complete=0;
        for(auto lib:{"Global","Frosty"}){auto arch=open_library(f.bytes(),lib);
            for(u32 i=0;i<arch.size();++i){auto entry=arch[i];if(entry.frequency<6 || entry.frequency>8)continue;
                ++total;out<<std::format("{}\t{}\t{}\t",lib,i,entry.frequency);
                try{auto raw=entry.unpack();auto c=rt_collection(rt_record_blob({raw}));size_t count=0;
                    for(auto& m:c.modules)count+=m.entries.size();
                     out<<std::format("{:016x}\t{}\t{}\t{}\tok",rt_key(c.blob),c.blob.size(),c.modules.size(),count);++complete;
                 }catch(const Error& e){out<<"-\t-\t-\t-\t"<<e.what();}
                 out<<'\t';for(auto b:arch.shader_hash(i).data)out<<std::format("{:02x}",b);out<<'\n';
            }
        }
        std::cout<<complete<<"/"<<total<<" packaged RT collections indexed\n";return complete==total?0:3;
    }
    if(cmd=="audit" && argc>=4) {
        unsigned jobs=6;bool dxil=false,strict=false;
        for(int i=4;i<argc;++i) {
            if(std::string_view(argv[i])=="--dxil")dxil=true;
            else if(std::string_view(argv[i])=="--require-all")strict=dxil=true;
            else if(std::string_view(argv[i])=="--jobs" && ++i<argc){
                std::string_view s=argv[i];auto [end,err]=std::from_chars(s.data(),s.data()+s.size(),jobs);
                if(err!=std::errc{} || end!=s.data()+s.size())return usage();
            }else return usage();
        }
        Mapping pak(argv[2]);bool complete=audit(pak.bytes(),argv[3],jobs,dxil);
        return strict && !complete?3:0;
    }
    if(cmd=="specialize" && (argc==4||argc==5)) {
        // Binary stdin/stdout protocol for the launcher. Only compile the
        // specialization the pipeline actually uses; no archive scan.
        Buffer raw{std::istreambuf_iterator<char>(std::cin),{}};
        if(raw.size()<32 || raw.size()>16*1024*1024)throw Error("invalid shader size");
        auto s=inspect_shader({raw});
        bool rectangle=std::string_view(argv[2])=="rectlist";
        if(s.dxbc.size()!=raw.size() || (rectangle?s.stage!=isa::vertex:(s.stage!=unsigned(argv[2][0]-'0') || argv[2][1])))
            throw Error("stage/container mismatch");
        unsigned caps=0;
        if(argc==5){std::string_view v=argv[4];auto [e,ec]=std::from_chars(v.data(),v.data()+v.size(),caps);
            if(ec!=std::errc{} || e!=v.end() || caps>15 || s.stage!=isa::compute)throw Error("invalid compute capability mask");}
        auto t=rectangle?isa::rectangle_assembly(s):isa::recompile(s,caps);
        auto dxil=isa::compile_dxil(t,std::string_view(argv[3])=="-"?"":argv[3]);
        u32 header[]{u32(t.views.size()),u32(dxil.size())};
        std::cout.write(reinterpret_cast<const char*>(header),sizeof(header));
        if(!t.views.empty())std::cout.write(reinterpret_cast<const char*>(t.views.data()),t.views.size()*4);
        std::cout.write(reinterpret_cast<const char*>(dxil.data()),dxil.size());
        return !std::cout;
    }
    if (cmd=="lookup" && argc>=4) {
        Mapping pak(argv[2]);
        std::set<u64> keys;
        for (int i=3;i<argc;++i) {
            std::string_view arg=argv[i];u64 key{};
            auto [end,ec]=std::from_chars(arg.data(),arg.data()+arg.size(),key,16);
            if(ec!=std::errc{} || end!=arg.data()+arg.size()) throw Error("invalid hexadecimal shader key");
            keys.insert(key);
        }
        for(auto lib:{"Global","Frosty"}) {
            auto archive=open_library(pak.bytes(),lib);
            for(u32 i=0;i<archive.size() && !keys.empty();++i) {
                auto record=archive[i].unpack();auto s=inspect_shader({record});
                auto key=s.key();
                if(!keys.erase(key)) continue;
                std::cout<<std::format("{:016x} {}[{}]: ",key,lib,i);
                try { isa::recompile(s);std::cout<<"lowered\n"; }
                catch(const Unsupported& e) {std::cout<<e.what()<<'\n';}
            }
        }
        for(auto k:keys) std::cout<<std::format("{:016x}: not found\n",k);
        return !keys.empty();
    }
    if (!(((cmd == "inventory" || cmd == "prepare") && argc == 4) || ((cmd == "inspect" || cmd=="disasm") && argc == 5) ||
          ((cmd == "lift" || cmd == "dump") && argc == 6))) return usage();
    Mapping pak(argv[2]);
    if (cmd == "prepare") { prepare(pak.bytes(), argv[3]); return 0; }
    auto archive = open_library(pak.bytes(), argv[3]);
    if (cmd == "inventory") {
        std::map<std::string_view, unsigned> counts;
        for (u32 i = 0; i < archive.size(); ++i) {
            auto record = archive[i].unpack();
            ++counts[inspect_shader({record}).format()];
        }
        std::cout << argv[3] << ": " << archive.size() << " entries\n";
        for (auto [name, n] : counts) std::cout << "  " << n << "  " << name << '\n';
        return 0;
    }
    u32 index{};
    std::string_view text = argv[4];
    auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), index);
    if (err != std::errc{} || end != text.data() + text.size()) throw Error("invalid shader index");
    auto entry = archive[index];
    auto record = entry.unpack();
    auto shader = inspect_shader({record});
    std::cout << std::format("{}[{}]: UE frequency {} | {} | DXBC {} bytes | vkd3d key {:016x}\n",
        argv[3], index, entry.frequency, shader.format(), shader.dxbc.size(), shader.key());
    if (cmd=="lift") emit(shader,argv[5],true);
     if(cmd=="dump"){
         fs::path dir=argv[5];auto stem=std::format("{:016x}",shader.key());fs::create_directories(dir);
         write_file(dir/(stem+".dxbc"),shader.dxbc.data.data(),shader.dxbc.size());
         if(entry.frequency>=6 && entry.frequency<=8)
             write_file(dir/(std::string(argv[3])+"-"+std::to_string(index)+".uecode"),record.data(),record.size());
        if(shader.xbch.size()){auto obj=shader.machine_object();write_file(dir/(stem+".fssm"),obj.data(),obj.size());}
    }
    if (cmd=="disasm") std::cout<<isa::listing(shader);
    if (cmd=="inspect" && shader.xbch.size()) {
        auto object=shader.machine_object();auto p=isa::interface(shader,object);
        std::cout<<std::format("stage {} | wave {} | threads {}x{}x{} | LDS {} bytes\n",isa::stage_name(p.stage),p.wave,
            p.threads[0],p.threads[1],p.threads[2],p.lds_bytes);
        for(auto& r:p.resources)std::cout<<std::format("  s{}+0x{:x}: register {}, type {}, kind {}, stride {}\n",
            r.table,r.offset,r.reg,r.type,r.kind,r.stride);
    }
    return 0;
} catch (const Unsupported& e) {
    std::cerr<<"frosty-shader: "<<e.what()<<"\n";
    return argc>1 && std::string_view(argv[1])=="specialize"?3:1;
} catch (const std::exception& e) {
    std::cerr << "frosty-shader: " << e.what() << '\n';
    return 1;
}
