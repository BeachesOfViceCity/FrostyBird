#pragma once
#include <charconv>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace frosty::isa {
// Coalesce lane-private ALU on the same EXEC mask. Only move temporaries
// whose final use is inside the region. Scalar state, memory, derivatives
// and subgroup operations keep their original participation and position.
inline std::string active_regions(std::string text){
    using V=std::string_view;
    constexpr V guard="    if (active) ";
    if(text.find(guard)==text.npos)return text;
    struct Line{V s;unsigned temp{};bool pred{},eligible{};};
    std::vector<Line> lines;std::map<unsigned,size_t> last;
    V rest=text;
    while(!rest.empty()){
        auto nl=rest.find('\n');auto s=rest.substr(0,nl);auto& l=lines.emplace_back();l.s=s;
        for(size_t pos=0;(pos=s.find("tmp",pos))!=s.npos;){
            pos+=3;unsigned id{};auto [end,ec]=std::from_chars(s.data()+pos,s.data()+s.size(),id);
            if(ec==std::errc{}){last[id]=lines.size()-1;pos=size_t(end-s.data());}
        }
        bool local=s.ends_with(';') && !s.contains('{') && !s.contains('}');
        if(local)for(V op:{"Wave","ANY(","group_","Quad","ddx","ddy","lds[","xs_lds_","Interlocked","res","UAV_","xs_visibility","xs_closest","TraceRay","Barrier","Load","Sample","Store"})
            if(s.contains(op)){local=false;break;}
        if(local && s.starts_with(guard)){
            auto dst=s.substr(guard.size());
            if(dst.starts_with('v') && dst.size()>1){unsigned id{};auto [end,ec]=std::from_chars(dst.data()+1,dst.data()+dst.size(),id);
                l.pred=l.eligible=ec==std::errc{} && V(end,dst.data()+dst.size()-end).starts_with(" = ");}
        }else if(local && s.starts_with("    ")){
            auto start=s.find(" tmp",4);
            if(start!=s.npos && start<=10){
                auto type=s.substr(4,start-4);
                if(type=="uint" || type=="uint2" || type=="uint3" || type=="uint4" || type=="bool" || type=="float" || type=="int"){
                    auto [end,ec]=std::from_chars(s.data()+start+4,s.data()+s.size(),l.temp);
                    l.eligible=ec==std::errc{} && l.temp && V(end,s.data()+s.size()-end).starts_with(" = ");
                }
            }
        }
        if(nl==rest.npos)break;
        rest.remove_prefix(nl+1);
    }
    std::string out;out.reserve(text.size());
    for(size_t i=0;i<lines.size();){
        if(!lines[i].eligible){out+=lines[i++].s;out+='\n';continue;}
        size_t j=i,guards=0;while(j<lines.size() && lines[j].eligible)guards+=lines[j++].pred;
        bool combine=guards>=4;
        for(auto k=i;combine && k<j;++k)if(lines[k].temp && last.at(lines[k].temp)>=j)combine=false;
        if(combine)out+="    [branch] if(active){\n";
        for(;i<j;++i){auto& l=lines[i];if(combine && l.pred){out+="    ";out+=l.s.substr(guard.size());}else out+=l.s;out+='\n';}
        if(combine)out+="    }\n";
    }
    return out;
}
}
