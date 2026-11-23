#include "archive.hpp"
#include "shader.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <format>
#include <iostream>

// Offline discovery only. Emit the pairs of graphics stages actually grouped
// by the cook, without assigning shader classes or inventing pipeline state.
int main(int argc,char** argv)try{
    using namespace frosty;if(argc!=2){std::cerr<<"frosty-material-index <Frosty-XSX.pak>\n";return 2;}
    int fd=open(argv[1],O_RDONLY|O_CLOEXEC);if(fd<0)throw Error("cannot open Pak");
    struct stat st{};if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0){close(fd);throw Error("invalid Pak");}
    auto data=mmap(nullptr,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);if(data==MAP_FAILED)throw Error("cannot map Pak");
    struct Mapped{void* p;size_t n;~Mapped(){munmap(p,n);}} mapped{data,size_t(st.st_size)};
    auto a=open_library({{static_cast<const u8*>(data),mapped.n}},"Frosty");
    std::vector<std::string> keys(a.size());
    for(u32 i=0;i<a.size();++i){
        auto e=a[i];if(e.frequency!=0 && e.frequency!=3)continue;
        auto raw=e.unpack();auto s=inspect_shader({raw});if(!s.xbch.size() || !s.bitcode.tag(0,"NOBC") || s.stage>1)continue;
        keys[i]=std::format("{}:{:016x}",s.stage,s.key());
    }
    for(u32 m=0;m<a.map_count();++m){
        std::cout<<m;auto ids=a.map_indices(m);
        for(u32 n=0;n<ids.size()/4;++n){auto& k=keys.at(ids.at<u32>(4ull*n));if(!k.empty())std::cout<<'\t'<<k;}
        std::cout<<'\n';
    }
    return !std::cout;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
