#include "rt_collection.hpp"
#include "isa.hpp"
#include <algorithm>
#include <format>
#include <set>

namespace frosty {
std::string rt_listing(const RtModule& m,const RtEntry& e){
    auto end=u32(m.code.size());
    for(auto& s:m.entries)if(s.offset>e.offset)end=std::min(s.offset,end);
    // The first 24 bytes of every linked entry are the reserved export
    // preamble. Stop at control transfers, not apparent terminators in data.
    std::set<u32> work{e.offset+24};std::map<u32,std::string> lines;
    while(!work.empty()){
        auto pc=*work.begin();work.erase(work.begin());
        while(!lines.contains(pc)){
            if(pc>=end || pc<e.offset+24)throw Unsupported("RT branch exits its entry");
            auto i=isa::decode(m.code,pc,32);auto next=pc+i.size;
            if(next>end)throw Unsupported("RT instruction crosses entry boundary");
            auto line=std::format("{:05x}  {}",pc,i.op);
            for(auto& a:i.args)line+=" "+a;
            if(!i.modifiers.empty())line+=" "+i.modifiers;
            lines.emplace(pc,line+"\n");
            if(i.op=="s_setpc_b64" || i.op=="s_endpgm")break;
            if(i.op=="s_branch" || i.op.starts_with("s_cbranch_")){
                int delta=std::bit_cast<std::int16_t>(std::uint16_t(m.code.at<u32>(pc)));
                work.insert(u32(std::int64_t(next)+4ll*delta));
                if(i.op=="s_branch")break;
            }
            pc=next;
        }
    }
    std::string text;for(auto& [_,l]:lines)text+=l;return text;
}
}
