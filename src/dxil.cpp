#include "isa.hpp"
#include "view_fields.hpp"
#include <charconv>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <format>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/SHA256.h>
#include <dxc/dxcapi.h>

namespace {
frosty::Buffer compile_hlsl(std::string_view source,std::vector<const wchar_t*> args){
    using namespace frosty;
    thread_local CComPtr<IDxcCompiler3> compiler;
    if (!compiler && FAILED(DxcCreateInstance(CLSID_DxcCompiler,IID_PPV_ARGS(&compiler))))
        throw Error("could not initialize host DXC");
    const DxcBuffer input{source.data(),source.size(),DXC_CP_UTF8};CComPtr<IDxcResult> result;
    HRESULT status=E_FAIL;
    if (FAILED(compiler->Compile(&input,args.data(),args.size(),nullptr,IID_PPV_ARGS(&result))) ||
        FAILED(result->GetStatus(&status)))throw Error("DXC invocation failed");
    if(FAILED(status)){
        CComPtr<IDxcBlobUtf8> errors;result->GetOutput(DXC_OUT_ERRORS,IID_PPV_ARGS(&errors),nullptr);
        throw Error(errors?errors->GetStringPointer():"DXC rejected generated shader");
    }
    CComPtr<IDxcBlob> code;
    if(FAILED(result->GetOutput(DXC_OUT_OBJECT,IID_PPV_ARGS(&code),nullptr)) || !code)throw Error("DXC returned no shader object");
    auto first=static_cast<const u8*>(code->GetBufferPointer());return {first,first+code->GetBufferSize()};
}
frosty::Buffer cached_hlsl(std::string_view source,const std::vector<const wchar_t*>& args){
    using namespace frosty;
    bool report=std::getenv("FROSTY_DXC_CACHE_REPORT")!=nullptr;
    // Original Xbox bytes are always lowered again for a new translator
    // build. Reuse DXC output only when the resulting HLSL and every DXC
    // argument are byte-identical, under the same libdxcompiler digest.
    std::filesystem::path file;
    if(auto dir=std::getenv("FROSTY_DXIL_SOURCE_CACHE");dir && *dir){
        llvm::SHA256 hash;hash.update(llvm::StringRef(source));
        for(auto a:args)hash.update(llvm::ArrayRef<u8>(reinterpret_cast<const u8*>(a),(std::wcslen(a)+1)*sizeof(wchar_t)));
        std::string key;for(auto v:hash.final())key+=std::format("{:02x}",v);
        file=std::filesystem::path(dir)/key;
        std::ifstream f(file,std::ios::binary|std::ios::ate);auto size=f.tellg();
        if(size>=64 && size<=std::streamoff((16u<<20)+32)){
            std::array<u8,32> digest{};Buffer cached(static_cast<size_t>(size)-digest.size());f.seekg(0);
            f.read(reinterpret_cast<char*>(digest.data()),digest.size());f.read(reinterpret_cast<char*>(cached.data()),cached.size());
            if(f && Bytes{cached}.tag(0,"DXBC") && Bytes{cached}.at<u32>(24)==cached.size() &&
               llvm::SHA256::hash(llvm::ArrayRef<u8>(cached.data(),cached.size()))==digest){
                if(report)std::fputs("frosty/dxc-cache: hit\n",stderr);
                return cached;
            }
        }
    }
    if(report)std::fputs("frosty/dxc-cache: compile\n",stderr);
    auto code=compile_hlsl(source,args);auto* first=code.data();
    if(!file.empty()){
        auto tmp=file;tmp+=std::format(".{}.{}.tmp",getpid(),std::hash<std::thread::id>{}(std::this_thread::get_id()));
        auto size=code.size();auto digest=llvm::SHA256::hash(llvm::ArrayRef<u8>(first,size));
        std::ofstream f(tmp,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(digest.data()),digest.size());
        f.write(reinterpret_cast<const char*>(first),size);f.close();std::error_code ec;
        if(f)std::filesystem::rename(tmp,file,ec);
        if(!f || ec)std::filesystem::remove(tmp,ec); // an unavailable cache is not a compile failure
    }
    return code;
}
}
namespace frosty {
Buffer compile_rt_library(std::string_view s){return cached_hlsl(s,{L"-T",L"lib_6_3",L"-O3",L"-Ges",L"-Gis"});}
}
namespace frosty::isa {
Buffer compile_dxil(const Translation& t,std::string_view numeric) {
    std::wstring profile=t.stage==vertex?L"vs":t.stage==pixel?L"ps":t.stage==geometry?L"gs":L"cs";
    profile+=(t.required_wave||std::ranges::any_of(t.views,[](auto k){return k&view::atomic64;}))?L"_6_6":t.inline_rays?L"_6_5":t.native_half?L"_6_2":L"_6_0";
    bool hex=numeric.starts_with("x");if(hex)numeric.remove_prefix(1);
    if((hex || !numeric.empty()) && numeric.size()!=t.views.size()*(hex?2u:1u))throw Error("view count mismatch");
    std::vector<std::wstring> macros;
    for(size_t i=0;i<t.views.size();++i) {
        unsigned n=0;
        if(!numeric.empty()){
            auto s=numeric.substr(i*(hex?2:1),hex?2:1);
            auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),n,hex?16:10);
            if(ec!=std::errc{} || end!=s.data()+s.size() || (!(t.views[i]&(view::image|view::sampler)) && n>2))
                throw Error("invalid descriptor specialization");
        }
        macros.push_back(L"FROSTY_VIEW_"+std::to_wstring(i)+L"="+std::to_wstring(n));
    }
    std::vector<const wchar_t*> args{L"-T",profile.c_str(),L"-E",L"main",L"-O3",L"-Ges",L"-Gis"};
    if(t.native_half)args.push_back(L"-enable-16bit-types");
    for(const auto& m:macros){args.push_back(L"-D");args.push_back(m.c_str());}
    return cached_hlsl(t.source,args);
}
} // namespace frosty::isa
