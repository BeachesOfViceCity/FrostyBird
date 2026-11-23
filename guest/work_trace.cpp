#define NOMINMAX
#include "work_trace.hpp"
#include <algorithm>
#include <cwchar>
WorkTrace::WorkTrace(){
    wchar_t dir[MAX_PATH]{},path[MAX_PATH+32]{};
    if(!GetEnvironmentVariableA("FROSTY_TRACE_WORK",nullptr,0) ||
       GetEnvironmentVariableW(L"FROSTY_CAPTURE_DIR",dir,MAX_PATH)>=MAX_PATH || !dir[0])return;
    std::swprintf(path,std::size(path),L"%ls\\gpu-work.tsv",dir);
    file_=_wfopen(path,L"wb");if(!file_)return;
    std::fputs("list_submission\tpresented\tqueue_type\tcommand\tshader1\tshader2\tshader3\tx\ty\tz\tretail_recording_stack_rvas\n",file_);
    base_=reinterpret_cast<UINT_PTR>(GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
    end_=base_+reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_+dos->e_lfanew)->OptionalHeader.SizeOfImage;
}
WorkTrace::~WorkTrace(){if(file_)std::fclose(file_);}
void WorkTrace::record(std::vector<RecordedWork>& list,ShaderKeys shaders,WorkKind kind,UINT x,UINT y,UINT z){
    if(!file_ || (!(shaders.a|shaders.b|shaders.c) && kind<WorkKind::blas))return;
    WorkKey key{shaders,kind,kind==WorkKind::tlas?x:0};
    {std::lock_guard g(lock_);auto it=reported_.find(key);if(it!=reported_.end() && it->second==presents_.load()/120)return;}
    if(std::any_of(list.begin(),list.end(),[&](auto& r){return r.key==key;}))return;
    RecordedWork item{key,x,y,z};void* pcs[40]{};auto n=CaptureStackBackTrace(1,UINT(std::size(pcs)),pcs,nullptr);
    for(USHORT j=0;j<n && item.n<item.rvas.size();++j){
        auto pc=reinterpret_cast<UINT_PTR>(pcs[j]);if(pc>=base_ && pc<end_)item.rvas[item.n++]=DWORD(pc-base_);
    }
    list.push_back(item);
}
void WorkTrace::submitted(UINT type,const std::vector<RecordedWork>& list){
    if(!file_ || list.empty())return;
    std::lock_guard g(lock_);auto frame=presents_.load(),epoch=frame/120;auto submission=++submissions_;
    constexpr const char* names[]{"draw","indexed","dispatch","indirect-draw","indirect-indexed","indirect-dispatch","indirect-other","build-blas","build-tlas","dispatch-rays"};
    for(const auto& r:list){
        auto [it,added]=reported_.try_emplace(r.key,epoch);if(!added && it->second==epoch)continue;
        it->second=epoch;auto s=r.key.shaders;
        std::fprintf(file_,"%llu\t%llu\t%u\t%s\t%016llx\t%016llx\t%016llx\t%u\t%u\t%u\t",submission,frame,type,names[UINT(r.key.kind)],s.a,s.b,s.c,r.x,r.y,r.z);
        for(UINT i=0;i<r.n;++i)std::fprintf(file_,"%s%08lx",i?",":"",r.rvas[i]);
        std::fputc('\n',file_);
    }
    std::fflush(file_);
}
