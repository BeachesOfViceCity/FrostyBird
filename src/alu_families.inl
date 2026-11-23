// Members of the instruction lowerer; inputs are architectural register
// words. Packed selectors and SCC are decoded here, not inferred from the
// function that happens to contain an instruction.
Str Lower::wide(const Str& operand,bool fp) const {
    Str s=operand;bool neg=false,abs=false;
    if(fp && s.starts_with('-') && s.size()>1 && (s[1]=='|' || s[1]=='s' || s[1]=='v')){s.erase(0,1);neg=true;}
    if(fp && s.starts_with('|') && s.ends_with('|')){s=s.substr(1,s.size()-2);abs=true;}
    Str e;
    if(s.starts_with("s[") || s.starts_with("v[")){
        auto r=registers(s);if(r.count!=2)unsupported("expected two register words");
        e=std::format("uint2({},{})",bits(reg(r.bank,r.first)),bits(reg(r.bank,r.first+1)));
    }else if(s.contains('.') && !s.starts_with("0x")){
        double d;auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),d);
        if(ec!=std::errc{} || end!=s.data()+s.size())unsupported("invalid double constant");
        auto b=std::bit_cast<u64>(d);e=std::format("uint2({}u,{}u)",u32(b),u32(b>>32));
    }else if(s=="null")e="uint2(0u,0u)";
    else if(!s.starts_with('s') && !s.starts_with('v')){
        if(fp && s.starts_with("0x"))e="uint2(0u,"+bits(s)+")"; // literal occupies the high word for FP64
        else e=std::format("uint2({},{})",bits(s),s.starts_with('-')?"~0u":"0u");
    }else unsupported("unknown 64-bit operand");
    if(abs)e="("+e+" & uint2(~0u,0x7fffffffu))";
    if(neg)e="("+e+" ^ uint2(0u,0x80000000u))";
    return e;
}
Str Lower::halfword(Str s,bool high) const {
    bool neg=false,abs=false;
    if(s.starts_with('-') && s.size()>1 && (s[1]=='|' || s[1]=='s' || s[1]=='v')){s.erase(0,1);neg=true;}
    if(s.starts_with('|') && s.ends_with('|')){s=s.substr(1,s.size()-2);abs=true;}
    Str v;
    if(s.contains('.') && !s.starts_with("0x"))v="f32tof16(asfloat("+bits(s)+"))";
    else v="(("+bits(s)+(high?" >> 16u)":" )")+" & 65535u)";
    if(abs)v="("+v+" & 32767u)";
    if(neg)v="("+v+" ^ 32768u)";
    return v;
}
bool Lower::compare_alu(const Instruction& i) {
    View op=i.op;const auto& a=i.args;
    bool x=op.starts_with("v_cmpx_"),vec=x||op.starts_with("v_cmp_"),imm=op.starts_with("s_cmpk_");
    if(!vec && !imm && !op.starts_with("s_cmp_"))return false;
    if(!i.modifiers.empty())unsupported("compare modifiers");
    auto suffix=op.substr(x||imm?7:6);auto k=suffix.rfind('_');if(k==View::npos)return false;
    View pred=suffix.substr(0,k),t=suffix.substr(k+1);
    unsigned j=vec&&!x?1:0;if(a.size()!=j+2)unsupported("compare operand count");
    if(vec && t=="u32" && (pred=="eq" || pred=="ne")){
        for(unsigned k=0;k<2;++k)if(auto v=value(a[j+k]);v.kind==Kind::ray_node && v.part==1 && a[j+1-k]=="0"){
            putmask(x?"exec":a[0],"active && ("+a[j+k]+(pred=="eq"?"==0u)":"!=0u)"));return true;
        }
        for(unsigned k=0;k<2;++k)if(value(a[j+k]).kind==Kind::front_face && a[j+1-k]=="0"){
            auto b="input."+field(p.inputs.at(value(a[j+k]).id));
            putmask(x?"exec":a[0],"active && "+(pred=="eq"?"!"+b:b));return true;
        }
    }
    bool fp=t.starts_with('f'),sign=t.starts_with('i');
    if(!fp && !sign && !t.starts_with('u'))return false;
    auto source=[&](unsigned n,bool raw){
        auto& s=a[j+n];Str v;
        if(t.ends_with("64")) {
            v=wide(s,fp);
            if(raw)return v;
            return (fp?"rd_double(":sign?"int64_t(rd_u64(":"rd_u64(")+v+(sign?"))":")");
        }
        if(t.ends_with("16")){
            v=halfword(s);
            if(raw)return v;
            return (fp?"f16tof32(":sign?"rd_s16(":"(")+v+")";
        }
        if(!t.ends_with("32"))unsupported("comparison type");
        v=bits(s);
        if(imm && n)v=sign?"asuint(rd_s16("+v+"))":"("+v+" & 65535u)";
        return raw?v:(fp?"asfloat(":sign?"asint(":"(")+v+")";
    };
    Str e;
    if(pred=="f")e="false";
    else if(pred=="t" || pred=="tru")e="true";
    else if(pred=="class" && fp) {
        auto raw=source(0,true);
        auto cl=t=="f64"?"rd_class64("+raw+")":t=="f16"?"rd_class("+raw+",15u,10u,31u)":"rd_class("+raw+",31u,23u,255u)";
        auto flags=t=="f16"?halfword(a[j+1]):bits(a[j+1]);
        e="(("+flags+" & (1u << "+cl+")) != 0u)";
    }else if(fp) {
        constexpr std::array<View,16> names{"f","lt","eq","le","gt","lg","ge","o","u","nge","nlg","ngt","nle","neq","nlt","tru"};
        auto it=std::ranges::find(names,pred);if(it==names.end())return false;
        auto test=unsigned(it-names.begin());
        if(t=="f32" && native_math())
            e=fast_fcmp32(source(0,true),source(1,true),test,native_fcmp_pair);
        else{
            auto widebits=[&](unsigned n){auto b=source(n,true);return t=="f64"?b:"uint2(0u,"+b+(t=="f16"?" << 16u)":")");};
            e=std::format("((rd_fp_relation({},{},0x{:08x}u) & {}u)!=0u)",widebits(0),widebits(1),
                          t=="f64"?0x7ff00000u:t=="f16"?0x7c000000u:0x7f800000u,test);
        }
    }else{
        auto l=source(0,false),r=source(1,false);
        View rel=pred=="eq"?"==":pred=="ne"||pred=="lg"?"!=":pred=="lt"?"<":pred=="le"?"<=":pred=="gt"?">":pred=="ge"?">=":"";
        if(rel.empty())return false;
        e="("+l+" "+Str(rel)+" "+r+")";
    }
    if(vec){
        bool same=!masked && uniform(a[j]) && uniform(a[j+1]);
        if(x)putmask("exec","active && ("+e+")"); // gfx10 CMPX writes EXEC, not VCC
        else {
            bool uniform_inputs=uniform(a[j]) && uniform(a[j+1]);
            auto r=maskname(a[0]);
            // Under a narrowed EXEC the ballot is not uniform, but the
            // truth value for every active lane can still be an SGPR-only
            // expression. Preserve it for CNDMASK -> READFIRSTLANE. Take
            // the snapshot before overwriting an aliased comparison input.
            Str rep;
            if(masked && uniform_inputs && !virtual_wave)rep=temp("bool",e);
            putmask(a[0],"active && ("+(rep.empty()?e:rep)+")");
            auto& dest=state[r];if(dest.kind==Kind::predicate)dest.uniform=same;
            if(!rep.empty() && dest.kind==Kind::predicate){
                representatives.insert(r);vars.insert("uniform_"+r);line("uniform_"+r+" = uint("+rep+");");
            }
        }
    }else write("scc","("+e+") ? 1u : 0u",scalar);
    return true;
}
void Lower::scalar_dest(const Str& dest,const Str& expression){
    if(dest=="null")return; // any SCC result has already been emitted
    if(dest=="m0"){write(dest,expression,scalar);return;}
    if(dest=="vcc_lo"||dest=="vcc_hi"){
        // SMEM and READFIRSTLANE can explicitly name the physical SGPR
        // halves of VCC. Save the full word as well as its lane predicate.
        // A subsequent vector comparison replaces the predicate/word.
        auto q=temp("uint",expression);bool hi=dest=="vcc_hi";
        auto other=hi?"word_vcc_lo":"word_vcc_hi";auto old=peek(other);
        if(virtual_wave && !(p.wave==32 && hi)){
            Str bit="(("+q+" >> (lane&31u)) & 1u)!=0u";vars.insert("vcc");
            if(p.wave==64)bit=std::format("((lane>=32u)=={} ? ({}) : vcc!=0u)",hi?"true":"false",bit);
            putmask("vcc",bit);
        }else if(!virtual_wave)set("vcc",{}); // only numeric uses have a portable meaning until the next compare
        state[other]=old;write("word_"+dest,q,scalar);return;
    }
    if(dest=="exec_lo"||dest=="exec_hi"){
        if(!virtual_wave)unsupported("numeric mask write requires a guest wave");
        bool hi=dest.ends_with("_hi");if(hi && p.wave==32)return;
        auto q=temp("uint",expression);Str v="(("+q+" >> (lane&31u)) & 1u)!=0u";
        bool exec=dest.starts_with("exec");
        if(p.wave==64)v=std::format("((lane>=32u)=={} ? ({}) : {})",hi?"true":"false",v,exec?"active":"(vcc!=0u)");
        if(!exec)vars.insert("vcc");
        putmask(exec?"exec":"vcc",v);return;
    }
    check_range(registers(dest),'s',1);write(dest,expression,scalar);
}
bool Lower::scalar_alu(const Instruction& i) {
    View op=i.op;auto& a=i.args;
    if(!op.starts_with("s_") || a.empty())return false;
    auto b=[&](unsigned j){return bits(a.at(j));};
    auto flag=[&]{if(value("scc").kind!=Kind::number)unsupported("unbound SCC input");return temp("uint","uint("+scalar_condition()+")");};
    Str e,cc;
    if(op=="s_add_u32" || op=="s_add_i32" || op=="s_sub_u32" || op=="s_sub_i32" || op=="s_addc_u32" || op=="s_subb_u32" || op=="s_addk_i32"){
        bool add=op.starts_with("s_add"),k=op=="s_addk_i32",is_signed=op.ends_with("i32");
        auto l=b(k?0:1),r=k?"asuint(rd_s16("+b(1)+"))":b(2);
        auto v=temp("uint",l+(add?" + ":" - ")+r);e=v;
        cc=is_signed?std::format("((({} ^ {}) & ({} ^ {})) >> 31u)",l,add?v:r,add?r:l,v):
            add?"uint("+v+" < "+l+")":"uint("+l+" < "+r+")";
        if(op=="s_addc_u32" || op=="s_subb_u32"){
            auto carry=flag();e=temp("uint",v+(add?" + ":" - ")+carry);
            cc="("+cc+" | uint("+ (add?e+" < "+v:v+" < "+carry)+"))";
        }
    }else if(op=="s_movk_i32" || op=="s_cmovk_i32" || op=="s_mulk_i32"){
        auto k="asuint(rd_s16("+b(1)+"))";
        if(op=="s_cmovk_i32")e=flag()+" ? "+k+" : "+b(0);
        else if(op=="s_mulk_i32")e=b(0)+" * "+k;
        else e=k;
    }else if(op=="s_cmov_b32")e=flag()+" ? "+b(1)+" : "+b(0);
    else if(op=="s_bitset0_b32" || op=="s_bitset1_b32")
        e=b(0)+(op=="s_bitset0_b32"?" & ~(1u << (":" | (1u << (")+b(1)+" & 31u))";
    else if(op=="s_bitcmp0_b32" || op=="s_bitcmp1_b32"){
        write("scc","(("+b(0)+" >> ("+b(1)+" & 31u)) & 1u)"+(op=="s_bitcmp0_b32"?" ^ 1u":""),scalar);return true;
    }else if(op=="s_bcnt0_i32_b32" || op=="s_bcnt1_i32_b32")e=Str("countbits(")+(op.starts_with("s_bcnt0")?"~":"")+b(1)+")",cc="@";
    else if(op=="s_ff0_i32_b32" || op=="s_ff1_i32_b32")e=Str("firstbitlow(")+(op.starts_with("s_ff0")?"~":"")+b(1)+")";
    else if(op=="s_flbit_i32")e="rd_clz_sign("+b(1)+")";
    else if(op=="s_flbit_i32_b32")e="rd_clz("+b(1)+")";
    else if(op=="s_sext_i32_i8" || op=="s_sext_i32_i16")e=Str("asuint(")+(op.ends_with("i8")?"rd_s8(":"rd_s16(")+b(1)+"))";
    else if(op=="s_abs_i32")e="asuint(abs(asint("+b(1)+")))",cc="@";
    else if(op=="s_absdiff_i32"){auto delta=temp("uint",b(1)+" - "+b(2));e="(asint("+delta+")<0 ? 0u-"+delta+" : "+delta+")";cc="@";}
    else if(op=="s_quadmask_b32")e="rd_quadmask("+b(1)+")",cc="@";
    else if(op=="s_mul_hi_i32")e="rd_smulhi("+b(1)+","+b(2)+")";
    else if(op=="s_bfm_b32")e=std::format("(((1u << ({} & 31u))-1u) << ({} & 31u))",b(1),b(2));
    else if(op=="s_bfe_u32" || op=="s_bfe_i32")e=std::format("{}({},{})",op.ends_with("i32")?"rd_ibs":"scalar_bfe",b(1),b(2)),cc="@";
    else if(op=="s_pack_ll_b32_b16" || op=="s_pack_lh_b32_b16" || op=="s_pack_hh_b32_b16"){
        auto l=b(1),r=b(2);e="(("+l+(op.starts_with("s_pack_hh")?" >> 16u)":" )")+" & 65535u) | ("+r+(op.starts_with("s_pack_ll")?" << 16u)":" & 0xffff0000u)");
    }else if(op.starts_with("s_lshl") && op.ends_with("_add_u32") && op.size()==15 && op[6]>='1' && op[6]<='4'){
        auto shifted=temp("uint",b(1)+" << "+Str(1,op[6])+"u");e=temp("uint",shifted+" + "+b(2));cc="uint("+e+" < "+shifted+")";
    }else if(op=="s_lshl_b32"||op=="s_lshr_b32"||op=="s_ashr_i32"){
        e=std::format("asuint({} {} ({} & 31u))",op=="s_ashr_i32"?"asint("+b(1)+")":b(1),op=="s_lshl_b32"?"<<":">>",b(2));cc="@";
    }else if(op=="s_and_b32"||op=="s_or_b32"||op=="s_xor_b32"||op=="s_andn2_b32"||op=="s_orn2_b32"||
             op=="s_nand_b32"||op=="s_nor_b32"||op=="s_xnor_b32"||op=="s_not_b32"){
        if(op=="s_not_b32")e="~"+b(1);
        else{
            auto rhs=(op.contains("n2_")?"~":"")+b(2);
            e="("+b(1)+(op.contains("and")?" & ":op.contains("xor")||op.contains("xnor")?" ^ ":" | ")+rhs+")";
            if(op.starts_with("s_n")||op.starts_with("s_xnor"))e="~"+e;
        }
        cc="@";
    }else return false;
    if(!i.modifiers.empty())unsupported("scalar arithmetic modifier");
    if(cc=="@"){e=temp("uint",e);cc="uint("+e+" != 0u)";}
    if(!cc.empty())write("scc",cc,scalar);
    scalar_dest(a[0],e);return true;
}
bool Lower::vector_alu(const Instruction& i) {
    View op=i.op;auto& a=i.args;if(!op.starts_with("v_"))return false;
    auto x=[&](unsigned j){return bits(a.at(j));};Str e;
    if(op=="v_xad_u32")e="("+x(1)+" ^ "+x(2)+") + "+x(3);
    else if(op=="v_xor3_b32")e=x(1)+" ^ "+x(2)+" ^ "+x(3);
    else if(op=="v_xnor_b32")e="~("+x(1)+" ^ "+x(2)+")";
    else if(op=="v_add_nc_i32"||op=="v_sub_nc_i32")e=x(1)+(op.starts_with("v_add")?" + ":" - ")+x(2);
    else if(op=="v_mad_u32_u24"||op=="v_mad_i32_i24"||op=="v_mul_i32_i24"||op=="v_mul_hi_i32_i24"||op=="v_mul_hi_u32_u24"){
        bool sign=op.contains("i24");auto w=[&](unsigned n){return sign?"asuint(asint("+x(n)+" << 8u) >> 8u)":"("+x(n)+" & 0xffffffu)";};
        e=op.contains("_hi_")?std::format("{}({},{})",sign?"rd_smulhi":"mulhi",w(1),w(2)):w(1)+" * "+w(2);
        if(op.starts_with("v_mad"))e+=" + "+x(3);
    }else if(op=="v_mul_hi_i32")e="rd_smulhi("+x(1)+","+x(2)+")";
    else if(op=="v_bfm_b32")e="(((1u << ("+x(1)+" & 31u))-1u) << ("+x(2)+" & 31u))";
    else if(op=="v_bfi_b32")e=std::format("((({0}) & {1}) | (~({0}) & {2}))",x(1),x(2),x(3));
    else if(op=="v_bfe_i32")e=std::format("rd_ibfe({},{},{})",x(1),x(2),x(3));
    else if(op=="v_ffbh_u32"||op=="v_ffbh_i32")e=std::format("{}({})",op.ends_with("i32")?"rd_clz_sign":"rd_clz",x(1));
    else if(op=="v_ffbl_b32")e="asuint(firstbitlow("+x(1)+"))";
    else if(op=="v_alignbit_b32")e=std::format("rd_align({},{},{})",x(1),x(2),x(3));
    else if(op=="v_alignbyte_b32")e=std::format("rd_alignbyte({},{},{})",x(1),x(2),x(3));
    else if(op=="v_lerp_u8")e=std::format("rd_lerp8({},{},{})",x(1),x(2),x(3));
    else if(op=="v_cvt_flr_i32_f32" || op=="v_cvt_rpi_i32_f32")e=std::format("cvt_i32(floor(asfloat({}){}))",x(1),op.contains("rpi")?" + 0.5":"");
    else if(op=="v_cvt_off_f32_i4")e="asuint(float(asint("+x(1)+" << 28u) >> 28) * 0.0625)";
    else if(op=="v_cvt_pk_i16_i32" || op=="v_cvt_pk_u16_u32"){
        auto cv=[&](unsigned n){return op.ends_with("i32")?"(asuint(clamp(asint("+x(n)+"),-32768,32767)) & 65535u)":"min("+x(n)+",65535u)";};
        e=cv(1)+" | ("+cv(2)+"<<16u)";
    }else if(op=="v_sat_pk_u8_i16")e=std::format("(uint(clamp(rd_s16({0}),0,255)) | (uint(clamp(asint({0})>>16,0,255))<<8u))",x(1));
    else if(op=="v_frexp_mant_f32"||op=="v_frexp_exp_i32_f32")e=std::format("rd_frexp({}).{}",x(1),op.contains("mant")?"x":"y");
    else if(op=="v_perm_b32")e=std::format("rd_perm({},{},{})",x(1),x(2),x(3));
    else if(op=="v_med3_i32"||op=="v_med3_u32"){
        auto v=[&](unsigned n){return op.ends_with("i32")?"asint("+x(n)+")":x(n);};
        e=std::format("asuint(max(min({0},{1}),min(max({0},{1}),{2})))",v(1),v(2),v(3));
    }else if(op=="v_sub_co_u32"||op=="v_subrev_co_u32"||op=="v_sub_co_ci_u32"||op=="v_subrev_co_ci_u32"){
        auto l=x(op.contains("rev")?3:2),r=x(op.contains("rev")?2:3);
        auto borrow=op.contains("_ci_")?temp("uint","uint("+mask(a.at(4))+")"):"0u";
        auto result=temp("uint",l+" - "+r+" - "+borrow);
        putmask(a[1],std::format("active && (({0} < {1}) || ({0} == {1} && {2} != 0u))",l,r,borrow));
        write(a[0],result,varying,true);return true;
    }else if(op=="v_sad_u32")e=std::format("(({0}>{1}?{0}-{1}:{1}-{0})+{2})",x(1),x(2),x(3));
    else if(op=="v_sad_u8"||op=="v_sad_hi_u8"||op=="v_sad_u16")e=std::format("(rd_sad({},{},{}u){} + {})",x(1),x(2),op.ends_with("u16")?16:8,op.contains("_hi_")?" * 65536u":"",x(3));
    else if(op=="v_msad_u8")e=std::format("(rd_msad({},{}) + {})",x(1),x(2),x(3));
    else if(op=="v_qsad_pk_u16_u8" || op=="v_mqsad_pk_u16_u8" || op=="v_mqsad_u32_u8"){
        bool full=op=="v_mqsad_u32_u8";auto dest=registers(a.at(0)),acc=registers(a.at(3));check_range(dest,'v',full?4:2);
        if(acc.count!=(full?4u:2u))unsupported("quad SAD accumulator");
        auto windows=temp("uint64_t","rd_u64("+wide(a[1])+")");std::array<Str,4> v;
        for(unsigned j=0;j<4;++j){
            auto old=bits(reg(acc.bank,acc.first+(full?j:j/2)));if(!full)old=std::format("(({}>>{}u)&65535u)",old,16*(j%2));
            auto win=std::format("uint({} >> {}u)",windows,8*j);
            v[j]=temp("uint",old+" + "+(op.starts_with("v_qsad")?"rd_sad("+win+","+x(2)+",8u)":"rd_msad("+win+","+x(2)+")"));
        }
        for(unsigned j=0;j<dest.count;++j)write(reg('v',dest.first+j),full?v[j]:"(("+v[j*2]+"&65535u) | ("+v[j*2+1]+"<<16u))",varying,true);
        return true;
    }
    else return false;
    if(!i.modifiers.empty())unsupported("integer arithmetic modifier");
    check_range(registers(a[0]),'v',1);
    write(a[0],e,std::all_of(a.begin()+1,a.end(),[&](auto& v){return uniform(v);})?scalar:varying,true);return true;
}

// GFX10 SDWA selects source and destination slices independently. Snapshot
// inputs before writing an overlapping VGPR, then reuse the ordinary opcode.
bool Lower::subword(const Instruction& i) {
    Instruction u=i;u.op.resize(u.op.size()-5);
    bool cmp=u.op.starts_with("v_cmp"),cmpx=u.op.starts_with("v_cmpx_");
    unsigned first=cmpx?0:1;
    if(u.args.size()<first+1 || u.args.size()>first+2)return false;
    std::array<Str,2> sel{"DWORD","DWORD"};Str dst="DWORD",unused="UNUSED_PAD";
    std::istringstream fields(i.modifiers);Str token;u.modifiers.clear();
    while(fields>>token) {
        if(token.starts_with("src0_sel:"))sel[0]=token.substr(9);
        else if(token.starts_with("src1_sel:"))sel[1]=token.substr(9);
        else if(token.starts_with("dst_sel:"))dst=token.substr(8);
        else if(token.starts_with("dst_unused:"))unused=token.substr(11);
        else if(token=="clamp" || token=="mul:2" || token=="mul:4" || token=="div:2")
            u.modifiers+=(u.modifiers.empty()?"":" ")+token;
        else unsupported("unhandled SDWA modifier "+token);
    }
    auto slice=[](const Str& s)->std::pair<unsigned,unsigned>{
        if(s=="DWORD")return {32,0};
        if(s=="WORD_0"||s=="WORD_1")return {16,unsigned(s.back()-'0')*16};
        if(s.size()==6 && s.starts_with("BYTE_") && s.back()>='0' && s.back()<='3')return {8,unsigned(s.back()-'0')*8};
        unsupported("unknown SDWA selector");
    };
    for(unsigned j=first;j<u.args.size();++j){
        auto s=u.args[j];bool sign=s.starts_with("sext(")&&s.ends_with(')');
        if(sign)s=s.substr(5,s.size()-6);
        bool neg=s.starts_with('-') && s.size()>1 && (s[1]=='v'||s[1]=='s'||s[1]=='|');
        if(neg)s.erase(0,1);
        bool ab=s.starts_with('|') && s.ends_with('|');if(ab)s=s.substr(1,s.size()-2);
        auto [width,shift]=slice(sel[j-first]);auto v=bits(s);
        if(width<32)v=std::format("(({} >> {}u) & {}u)",v,shift,(1u<<width)-1);
        if(sign && width<32)v=std::format("asuint(asint({} << {}u) >> {}u)",v,32-width,32-width);
        // VOP floating modifiers apply after source selection at the
        // precision of the selected instruction.
        bool half=u.op.ends_with("f16");
        if(ab)v="("+v+(half?" & 32767u)":" & 0x7fffffffu)");
        if(neg)v="("+v+(half?" ^ 32768u)":" ^ 0x80000000u)");
        auto t=temp("uint",v);set(t,uniform(s)?scalar:varying);u.args[j]=t;
    }
    auto [width,shift]=slice(dst);Str old;
    if(!cmp && width<32 && unused=="UNUSED_PRESERVE"){vars.insert(i.args[0]);old=temp("uint",i.args[0]);}
    if(!numeric(u))unsupported("unimplemented SDWA base "+u.op);
    if(!cmp && width<32){
        u32 mask=(1u<<width)-1;auto d=u.args[0];
        auto part=std::format("(({} & {}u) << {}u)",d,mask,shift);
        if(unused=="UNUSED_PRESERVE")part=std::format("(({} & 0x{:08x}u) | {})",old,~(mask<<shift),part);
        else if(unused=="UNUSED_SEXT")part=std::format("(asuint(asint({} << {}u) >> {}u) << {}u)",d,32-width,32-width,shift);
        else if(unused!="UNUSED_PAD")unsupported("unknown SDWA destination policy");
        write(d,part,varying,true);
    }
    return true;
}
bool Lower::half_alu(const Instruction& i) {
    auto& a=i.args;View name=i.op;if(!name.starts_with("v_") || a.size()<2)return false;
    if(name=="v_cvt_pkrtz_f16_f32")return false; // dedicated RTZ conversion
    bool packed=name.starts_with("v_pk_"),mix=name.starts_with("v_fma_mix");
    bool dot=name.starts_with("v_dot");
    if(!packed && !mix && !dot && !name.ends_with("_f16") && !name.ends_with("_u16") && !name.ends_with("_i16") && !name.ends_with("_b16") &&
       name!="v_cvt_f16_f32" && name!="v_fma_mix_f32")return false;
    unsigned inputs=a.size()-1;if(inputs>3)return false;
    unsigned lo=0,hi=mix?0u:7u,nlo=0,nhi=0;bool clamp=false;Str scale;
    std::istringstream m(i.modifiers);Str tok;
    while(m>>tok){
        if(tok=="clamp"){clamp=true;continue;}
        if(tok=="mul:2"||tok=="mul:4"||tok=="div:2"){scale=tok=="mul:2"?"2.0":tok=="mul:4"?"4.0":"0.5";continue;}
        auto pos=tok.find(":[");if(pos==Str::npos || !tok.ends_with(']'))unsupported("unhandled half modifier "+tok);
        unsigned mask=0,n=0;for(size_t j=pos+2;j+1<tok.size();j+=2){
            if(n==4 || (tok[j]!='0' && tok[j]!='1') || (j+2<tok.size() && tok[j+1]!=','))unsupported("invalid operand selector");
            mask|=unsigned(tok[j]-'0')<<n++;
        }
        auto key=tok.substr(0,pos);
        if(key=="op_sel")lo=mask;
        else if(key=="op_sel_hi")hi=mask;
        else if(key=="neg_lo")nlo=mask;
        else if(key=="neg_hi")nhi=mask;
        else unsupported("unhandled packed selector "+key);
    }
    auto raw=[&](unsigned n,bool upper){
        auto s=a.at(n+1);auto mask=upper?hi:lo;
        auto w=halfword(s,(mask>>n)&1);
        if(((upper?nhi:nlo)>>n)&1)w="("+w+" ^ 32768u)";
        return w;
    };
    auto f=[&](unsigned n,bool up){return "f16tof32("+raw(n,up)+")";};
    auto finishfp=[&](Str e){if(!scale.empty())e="(("+e+") * "+scale+")";return clamp?"saturate("+e+")":e;};
    auto preserve=[&](Str e,bool upper){
        auto d=a[0];vars.insert(d);
        write(d,upper?"("+d+" & 65535u) | (("+e+") << 16u)":"("+d+" & 0xffff0000u) | (("+e+") & 65535u)",varying,true);
    };
    if(mix){
        std::array<Str,3> src;
        for(unsigned j=0;j<3;++j){
            auto s=a.at(j+1);
            src[j]=(hi&(1u<<j))?"f16tof32("+halfword(s,(lo>>j)&1)+")":"asfloat("+bits(s)+")";
            if(nhi&(1u<<j))src[j]="abs("+src[j]+")";
            if(nlo&(1u<<j))src[j]="(-"+src[j]+")";
        }
        auto e=finishfp(std::format("mad({},{},{})",src[0],src[1],src[2]));
        if(name=="v_fma_mix_f32")write(a[0],"asuint("+e+")",varying,true);
        else if(name=="v_fma_mixlo_f16"||name=="v_fma_mixhi_f16")preserve("f32tof16("+e+")",name.contains("mixhi"));
        else return false;
        return true;
    }
    if(dot) {
        bool acc=name.starts_with("v_dot2c_")||name.starts_with("v_dot4c_");
        if(a.size()!=(acc?3u:4u) || lo || nlo || nhi || !scale.empty() || clamp)return false;
        auto z=bits(a[acc?0:3]);
        if(name=="v_dot2_f32_f16"||name=="v_dot2c_f32_f16")
            write(a[0],std::format("asuint(mad({},{},mad({},{},asfloat({}))))",f(0,false),f(1,false),f(0,true),f(1,true),z),varying,true);
        else if(name=="v_dot2_i32_i16"||name=="v_dot2_u32_u16"||name=="v_dot4_i32_i8"||name=="v_dot4c_i32_i8"||
                name=="v_dot4_u32_u8"||name=="v_dot8_i32_i4"||name=="v_dot8_u32_u4")
            write(a[0],std::format("rd_dot({},{},{},{}u,{})",bits(a[1]),bits(a[2]),z,name.ends_with("16")?16:name.ends_with("8")?8:4,
                  name.contains("_i32_")?"true":"false"),varying,true);
        else return false;
        return true;
    }
    if(name=="v_cvt_f32_f16"){write(a[0],"asuint("+finishfp(f(0,false))+")",varying,true);return true;}
    if(name=="v_cvt_f16_f32"||name=="v_cvt_f16_i16"||name=="v_cvt_f16_u16"){
        auto v=name.ends_with("f32")?"asfloat("+bits(a[1])+")":name.ends_with("i16")?"float(rd_s16("+raw(0,false)+"))":"float("+raw(0,false)+")";
        preserve("f32tof16("+finishfp(v)+")",lo&8);return true;
    }
    if(name=="v_cvt_i16_f16"||name=="v_cvt_u16_f16"){
        auto v=f(0,false);preserve(name=="v_cvt_i16_f16"?"asuint(clamp(asint(cvt_i32("+v+")),-32768,32767))":"min(cvt_u32("+v+"),65535u)",lo&8);return true;
    }
    if(name=="v_pack_b32_f16"){write(a[0],raw(0,false)+" | ("+raw(1,false)+" << 16u)",varying,true);return true;}
    if(name=="v_frexp_mant_f16"||name=="v_frexp_exp_i16_f16"){
        auto parts=temp("uint2","rd_frexp(asuint("+f(0,false)+"))");
        preserve(name.contains("mant")?"f32tof16(asfloat("+parts+".x))":parts+".y",lo&8);return true;
    }
    if(name=="v_mad_u32_u16" || name=="v_mad_i32_i16"){
        auto cv=[&](unsigned j){return name.ends_with("i16")?"asuint(rd_s16("+raw(j,false)+"))":raw(j,false);};
        write(a[0],cv(0)+" * "+cv(1)+" + "+bits(a[3]),varying,true);return true;
    }
    auto middle=name.substr(packed?5:2);auto end=middle.rfind('_');
    if(end==View::npos)return false;
    auto op=middle.substr(0,end);bool fp=name.ends_with("f16");
    // Two binary16 results stay in one VGPR. Preserve each VOP3P selector
    // and sign bit before the arithmetic; pack the result by bitcast, not
    // by a second floating-point conversion. Non-native adapters and ops
    // with a separate output scale retain the widened implementation.
    if(p.native_half && !reference_queue && packed && fp && scale.empty() &&
        (op=="add" || op=="mul" || op=="fma" || op=="min" || op=="max")){
        auto pair=[&](unsigned j){return "float16_t2(asfloat16((uint16_t)("+raw(j,false)+")),asfloat16((uint16_t)("+raw(j,true)+")))";};
        auto q=pair(0),r=pair(1);Str e;
        if(op=="fma")e="mad("+q+","+r+","+pair(2)+")";
        else if(op=="min" || op=="max")e=Str(op)+"("+q+","+r+")";
        else e="("+q+(op=="add"?" + ":" * ")+r+")";
        if(clamp)e="saturate("+e+")";
        half_vectors=true;write(a[0],"xs_pack2("+e+")",varying,true);return true;
    }
    auto lane=[&](bool up)->Str{
        auto w=[&](unsigned n){return raw(n,up);};
        auto val=[&](unsigned n){return fp?f(n,up):name.ends_with("i16")?"rd_s16("+w(n)+")":w(n);};
        Str e;auto x=val(0);
        if(op=="add"||op=="add_nc"||op=="sub"||op=="sub_nc"||op=="mul"||op=="mul_lo")
            e=x+(op.starts_with("add")?" + ":op.starts_with("sub")?" - ":" * ")+val(1);
        else if(op=="subrev")e=val(1)+" - "+x;
        else if(op=="max"||op=="min")e=std::format("{}({},{})",op,x,val(1));
        else if(op=="max3"||op=="min3")e=std::format("{0}({0}({1},{2}),{3})",op.substr(0,3),x,val(1),val(2));
        else if(op=="med3")e=std::format("max(min({0},{1}),min(max({0},{1}),{2}))",x,val(1),val(2));
        else if(op=="lshlrev"||op=="lshrrev"||op=="ashrrev")e=val(1)+(op=="lshlrev"?" << (":" >> (")+w(0)+" & 15u)";
        else if(op=="mad"&&!fp)e=x+" * "+val(1)+" + "+val(2);
        else if(fp && (op=="fma"||op=="fmac"||op=="fmaak"||op=="fmamk")){
            auto z=op=="fmac"?"f16tof32("+halfword(a[0],up)+")":val(2);
            e=std::format("mad({},{},{})",x,val(1),z);
        }else if(fp && (op=="rcp"||op=="sqrt"||op=="rsq"||op=="log"||op=="exp"||op=="floor"||op=="ceil"||op=="trunc"||op=="rndne"||op=="fract"||op=="sin"||op=="cos")){
            View fn=op=="rsq"?"rsqrt":op=="log"?"log2":op=="exp"?"exp2":op=="rndne"?"round":op=="fract"?"frac":op;
            e=std::format("{}({}{})",fn,x,op=="sin"||op=="cos"?"*6.283185307179586":"");
        }else if(fp && op=="ldexp")e="ldexp("+x+",rd_s16("+w(1)+"))";
        else return {};
        if(fp)return "f32tof16("+finishfp(e)+")";
        if(clamp && packed && (op=="add"||op=="sub"||op=="mad")){
            if(name.ends_with("i16"))e="clamp("+e+",-32768,32767)";
            else if(op=="sub")e="("+x+" >= "+val(1)+" ? "+e+" : 0u)";
            else e="min("+e+",65535u)";
        }else if(clamp||!scale.empty())unsupported("integer half saturation or scaling");
        return "(asuint("+e+") & 65535u)";
    };
    auto bottom=lane(false);if(bottom.empty())return false;
    if(packed)write(a[0],bottom+" | ("+lane(true)+" << 16u)",varying,true);
    else preserve(bottom,lo&8);
    return true;
}
bool Lower::atomic_op(View op,Str location,Str raw,unsigned id,Range data,bool returns) {
    check_range(data,'v',op=="cmpswap"?2:1);auto v=reg('v',data.first);
    atomic32(op,location,raw,id,bits(v),op=="cmpswap"?bits(reg('v',data.first+1)):"",returns?v:"");
    return true;
}
void Lower::atomic32(View op,const Str& addr,const Str& raw,unsigned id,const Str& value,const Str& extra,const Str& dest) {
    bool typed=id!=~0u;
    if(typed){
        view_width(id,1,true);
        integer_views.insert(id);
    }
    auto cast=[&](Str v){return typed?std::format("BITS{}({})",id,v):v;};
    if(masked)line("if(active){");
    auto old=temp(typed?std::format("TYPE{}",id):"uint","0");
    auto call=[&](View fn,const Str& args){line(std::format("{}Interlocked{}({},{},{});",raw,fn,addr,args,old));};
    if(op=="add"||op=="sub"||op=="and"||op=="or"||op=="xor"||op=="swap")
        call(op=="and"?"And":op=="or"?"Or":op=="xor"?"Xor":op=="swap"?"Exchange":"Add",cast(op=="sub"?"(0u-"+value+")":value));
    else if(op=="cmpswap")call("CompareExchange",cast(extra)+","+cast(value));
    else{
        auto expected=temp("uint","0u");Str next;
        if(op=="smin"||op=="smax")next=std::format("asuint({}(asint({}),asint({})))",op=="smin"?"min":"max",expected,value);
        else if(op=="umin"||op=="umax")next=std::format("{}({},{})",op=="umin"?"min":"max",expected,value);
        else if(op=="fmin"||op=="fmax")next=std::format("asuint({}(asfloat({}),asfloat({})))",op=="fmin"?"min":"max",expected,value);
        else if(op=="inc")next=std::format("({0}>={1}?0u:{0}+1u)",expected,value);
        else if(op=="dec")next=std::format("({0}==0u || {0}>{1}?{1}:{0}-1u)",expected,value);
        else if(op=="rsub")next=value+" - "+expected;
        else if(op=="csub")next=std::format("({0}<{1}?0u:{0}-{1})",expected,value);
        else if(op=="mskor")next="("+expected+" & ~"+value+") | "+extra;
        else if(op=="wrap")next=std::format("({0}>={1}?{0}-{1}:{0}+{2})",expected,value,extra);
        else if(op=="fcmpswap")next=std::format("(rd_fp_relation(uint2(0u,{}),uint2(0u,{}),0x7f800000u)==2u?{}:{})",expected,extra,value,expected);
        else if(op=="fadd")next="asuint(asfloat("+expected+") + asfloat("+value+"))";
        else unsupported("unimplemented atomic operation "+Str(op));
        // A bitwise CAS loop handles unsigned/signed order independently of
        // the bound typed UAV's numeric class. Never use a non-atomic load.
        line("[allow_uav_condition] do {");
        line(expected+" = asuint("+old+");");
        call("CompareExchange",cast(expected)+","+cast(next));
        line("} while(asuint("+old+") != "+expected+");");
    }
    if(!dest.empty())write(dest,"asuint("+old+")",varying);
    if(masked)line("}");
}
bool Lower::lds_io(const Instruction& i) {
    View op=i.op;auto& a=i.args;
    bool load=op.starts_with("ds_read"),storeop=op.starts_with("ds_write");
    if(!load && !storeop)return false;
    if(!virtual_wave || !p.lds_bytes)unsupported("LDS transfer without mapped workgroup storage");
    op.remove_prefix(load?7:8);
    bool pair=op.starts_with('2');if(pair)op.remove_prefix(1);
    bool st64=op.starts_with("st64");if(st64)op.remove_prefix(4);
    if(!op.starts_with('_'))return false;
    op.remove_prefix(1);
    bool half=op.ends_with("_d16"),hi=op.ends_with("_d16_hi");
    if(hi)op.remove_suffix(7);else if(half)op.remove_suffix(4);
    bool sign=op.starts_with('i');
    unsigned width=op=="b128"?128:op=="b96"?96:op=="b64"?64:op=="b32"?32:
        op=="b16"||op=="i16"||op=="u16"?16:op=="b8"||op=="i8"||op=="u8"?8:0;
    if(!width && op=="addtid_b32"){
        if(a.size()!=1 || value("m0").kind!=Kind::number)unsupported("ADDTID requires numeric M0");
        unsigned off=p.code.at<u32>(i.pc)&65535u;if(i.modifiers.contains("gds"))unsupported("GDS address space");
        auto loc=std::format("lds[((m0 & 65535u) + {}u + 4u*lane) >> 2u]",off);
        begin_lds(load);if(load)load_values(registers(a[0]),loc);else store(loc+" = "+bits(a[0]));
        end_lds();return true;
    }
    if(!width)return false;
    unsigned words=std::max(1u,width/32);
    if((pair && width!=32 && width!=64) || words*(1u+pair)>4 ||
       a.size()!=(load?2u:2u+pair))unsupported("extended LDS transfer");
    auto address=temp("uint",bits(a.at(load?1:0)));
    unsigned offsets[2]{};std::istringstream fields(i.modifiers);Str t;
    while(fields>>t) {
        if(pair && (t.starts_with("offset0:")||t.starts_with("offset1:")))
            offsets[t[6]-'0']=number(View(t).substr(8))*(width/8)*(st64?64:1);
        else if(!pair && t.starts_with("offset:"))offsets[0]=number(View(t).substr(7));
        else unsupported("extended LDS transfer modifier");
    }
    Range dst{};if(load){dst=registers(a[0]);check_range(dst,'v',words*(1u+pair));}
    begin_lds(load);
    std::vector<Str> result;
    for(unsigned k=0;k<=unsigned(pair);++k){
        Range src{};if(!load){src=registers(a[1+k]);check_range(src,'v',words);}
        for(unsigned j=0;j<words;++j){
            auto byte=std::format("({}+{}u)",address,offsets[k]+4*j);
            auto loc="lds["+byte+">>2u]";
            if(width>=32){
                if(half_lds){
                    if(load)result.push_back("xs_lds_word("+byte+")");
                    else line("xs_lds_store_word("+byte+","+bits(reg('v',src.first+j))+",active,lane);");
                }else if(load)result.push_back(loc);else store(loc+" = "+bits(reg('v',src.first+j)));
            }else{
                u32 mask=(1u<<width)-1;auto shift="(("+byte+" & 3u)*8u)";
                if(load){
                    auto val=half_lds?"xs_lds_half("+byte+")":std::format("(({} >> {}) & {}u)",loc,shift,mask);
                    if(sign)val=std::format("asuint(asint({}<<{}u)>>{}u)",val,32-width,32-width);
                    if(half||hi){
                        auto d=reg('v',dst.first);vars.insert(d);
                        val=hi?"(("+d+" & 65535u) | ("+val+" << 16u))":"(("+d+" & 0xffff0000u) | ("+val+" & 65535u))";
                    }
                    result.push_back(val);
                }else if(half_lds){
                    line("xs_lds_store_half("+byte+",("+bits(reg('v',src.first))+" >> "+std::to_string(hi?16:0)+"u),active,lane);");
                }else{
                    auto val=std::format("((({} >> {}u) & {}u) << {})",bits(reg('v',src.first)),hi?16:0,mask,shift);
                    atomic32("mskor",loc,"",~0u,std::format("({}u << {})",mask,shift),val,"");
                }
            }
        }
    }
    if(load){
        Str e;for(const auto& v:result){if(!e.empty())e+=",";e+=v;}
        load_values(dst,result.size()==1?e:"uint"+suffix(unsigned(result.size()))+"("+e+")");
    }
    end_lds();return true;
}
bool Lower::wide_lds_atomic(const Instruction& i) {
    View op=i.op;auto& a=i.args;bool pair=op.starts_with("ds_wrxchg2");
    bool wide64=op.ends_with("64");if(!wide64 && !pair)return false;
    if(!virtual_wave || !p.lds_bytes || i.modifiers.contains("gds"))unsupported("unmapped wide data-share atomic");
    bool ret=op.contains("_rtn_"),cas=op.starts_with("ds_cmpst"),msk=op.starts_with("ds_mskor");
    if(a.size()!=2u+ret+(cas||msk||pair))unsupported("wide atomic operand count");
    unsigned offs[2]{},words=wide64?2:1;std::istringstream m(i.modifiers);Str token;
    while(m>>token){
        if(!pair && token.starts_with("offset:"))offs[0]=number(View(token).substr(7));
        else if(pair && (token.starts_with("offset0:")||token.starts_with("offset1:")))
            offs[token[6]-'0']=number(View(token).substr(8))*words*4*(op.contains("st64")?64:1);
        else unsupported("wide LDS offset modifier");
    }
    auto addr=temp("uint",bits(a[ret]));Range dst{};
    if(ret){dst=registers(a[0]);check_range(dst,'v',words*(1+unsigned(pair)));}
    auto src=[&](unsigned j){return wide64?"rd_u64("+wide(a[1+ret+j])+")":bits(a[1+ret+j]);};
    Str ty=wide64?"uint64_t":"uint";auto v=temp(ty,src(0));
    auto w=cas||msk||pair?temp(ty,src(1)):"";
    std::vector<Str> olds;for(unsigned j=0;j<=unsigned(pair);++j)olds.push_back(temp(ty,"0u"));
    // The workgroup interpreter only schedules one guest wave at a time.
    // Serialize this instruction's participating lanes for indivisible
    // multiword updates; SM6.0 has no portable 64-bit LDS CAS.
    line("GroupMemoryBarrierWithGroupSync();");
    line("[loop] for(uint owner=0u;owner<LANES;++owner){ if(active && lane==owner){");
    for(unsigned j=0;j<=unsigned(pair);++j){
        auto at=temp("uint",std::format("({}+{}u)>>2u",addr,offs[j]));
        auto loc="lds["+at+"]",old=olds[j];
        line(old+" = "+(wide64?"rd_u64(uint2("+loc+",lds["+at+"+1u]))":loc)+";");
        auto cmp=[&](Str l,Str r,View rel){
            if(op.ends_with("f64"))return std::format("(rd_fp_relation(rd_words({}),rd_words({}),0x7ff00000u)=={}u)",l,r,rel=="<"?1:rel=="=="?2:4);
            if(op.ends_with("i64")){l="int64_t("+l+")";r="int64_t("+r+")";}
            return "("+l+Str(rel)+r+")";
        };
        Str e;
        if(op.starts_with("ds_wrxchg"))e=j?w:v;
        else if(op.starts_with("ds_add"))e=old+"+"+v;
        else if(op.starts_with("ds_sub"))e=old+"-"+v;
        else if(op.starts_with("ds_rsub"))e=v+"-"+old;
        else if(op.starts_with("ds_inc"))e=std::format("({0}>={1}?0u:{0}+1u)",old,v);
        else if(op.starts_with("ds_dec"))e=std::format("({0}==0u||{0}>{1}?{1}:{0}-1u)",old,v);
        else if(op.starts_with("ds_and"))e=old+"&"+v;
        else if(op.starts_with("ds_or"))e=old+"|"+v;
        else if(op.starts_with("ds_xor"))e=old+"^"+v;
        else if(msk)e="("+old+"&~"+v+")|"+w;
        else if(cas)e=cmp(old,v,"==")+"?"+w+":"+old;
        else if(op.starts_with("ds_min")||op.starts_with("ds_max"))e=cmp(v,old,op.starts_with("ds_min")?"<":">")+"?"+v+":"+old;
        else if(op=="ds_condxchg32_rtn_b64")e=std::format("rd_u64(uint2(asint(uint({0}))<0?uint({0}):uint({1}),asint(uint({0}>>32u))<0?uint({0}>>32u):uint({1}>>32u)))",v,old);
        else unsupported("unsupported wide LDS operation");
        auto n=temp(ty,e);line(loc+" = uint("+n+");");if(wide64)line("lds["+at+"+1u] = uint("+n+">>32u);");
    }
    line("} GroupMemoryBarrierWithGroupSync(); }");
    if(ret)for(unsigned j=0;j<olds.size();++j)for(unsigned k=0;k<words;++k)
        write(reg('v',dst.first+words*j+k),"uint("+olds[j]+(k?">>32u)":")"),varying,true);
    return true;
}
bool Lower::wide_alu(const Instruction& i) {
    View op=i.op;auto& a=i.args;
    if((!op.starts_with("s_")&&!op.starts_with("v_")) || !op.contains("64") || a.empty() || !i.modifiers.empty())return false;
    if(op.starts_with("v_cmp")||op.starts_with("s_cmp")||op.contains("saveexec")||op.contains("wrexec")||op.contains("getpc")||
       op.contains("setpc")||op.contains("swappc")||op.contains("wqm"))return false;
    bool vec=op.starts_with("v_");
    auto pred=[&](const Str& s){
        if(s=="exec"||s=="vcc"||s=="exec_lo"||s=="vcc_lo")return true;
        if(!s.starts_with("s["))return false;
        auto r=registers(s);return value(reg('s',r.first)).kind==Kind::predicate;
    };
    if(!vec && (a[0]=="exec" || a[0]=="vcc" || std::any_of(a.begin()+1,a.end(),pred)))return false;
    if(op=="s_mov_b64" && a.size()==2 && a[0].starts_with("s[") && a[1].starts_with("s[")){
        auto src=registers(a[1]),dst=registers(a[0]);check_range(src,'s',2);check_range(dst,'s',2);
        // A register-pair move can spill the user table pointer and its
        // zero upper word. Preserve each half's provenance independently.
        std::array<Value,2> values;std::array<Str,2> data;
        for(unsigned k=0;k<2;++k){auto n=reg('s',src.first+k);auto v=values[k]=value(n);
            if(v.kind==Kind::table || v.kind==Kind::descriptor){if(raw_sgprs)data[k]=temp("uint",n);}
            // A B64 move also copies a word the program may never observe.
            // Carry that word without inventing its value; bits(), mask()
            // and descriptor lookup still reject an unresolved consumer.
            else if(v.kind==Kind::unknown || v.kind==Kind::opaque_word)values[k]={Kind::opaque_word};
            else if(v.kind==Kind::prim)continue;
            else if(v.kind==Kind::code_lo || v.kind==Kind::code_hi)data[k]=temp("uint",n);
            else data[k]=temp("uint",bits(n));
        }
        for(unsigned k=0;k<2;++k){auto n=reg('s',dst.first+k);
            if(data[k].empty())set(n,values[k]);else write(n,data[k],values[k]);}
        return true;
    }
    if((op=="s_mov_b64" || op=="s_cselect_b64") &&
       std::ranges::all_of(a.begin()+1,a.end(),[](const Str& s){return s=="0"||s=="-1";}))return false;
    auto u=[&](unsigned j){return "rd_u64("+wide(a.at(j))+")";};
    auto d=[&](unsigned j){soft_double=true;return "rd_u64("+wide(a.at(j),true)+")";};
    auto b=[&](unsigned j){return bits(a.at(j));};
    Str e;bool two=true,cc=false;
    if(op=="s_bitreplicate_b64_b32")e="rd_spread("+b(1)+")";
    else if(op=="s_quadmask_b64"){
        auto v=temp("uint2",wide(a[1]));e=std::format("uint64_t(rd_quadmask({0}.x)|(rd_quadmask({0}.y)<<8u))",v);cc=true;
    }else if(op=="v_mad_u64_u32" || op=="v_mad_i64_i32"){
        auto dst=registers(a[0]);check_range(dst,'v',2);bool sg=op=="v_mad_i64_i32";
        auto cv=[&](unsigned j){return sg?"int64_t(asint("+b(j)+"))":"uint64_t("+b(j)+")";};
        auto prod=temp("uint64_t","uint64_t("+cv(2)+" * "+cv(3)+")"),acc=temp("uint64_t",u(4)),sum=temp("uint64_t",prod+"+"+acc);
        auto carry=sg?std::format("((({0}^{2})&({1}^{2}))>>63u)!=0u",prod,acc,sum):sum+"<"+prod;
        putmask(a[1],"active && ("+carry+")");
        write(reg('v',dst.first),"uint("+sum+")",varying,true);write(reg('v',dst.first+1),"uint("+sum+">>32u)",varying,true);return true;
    }else if(op=="s_mov_b64")e=u(1);
    else if(op=="s_cmov_b64"||op=="s_cselect_b64"){
        if(value("scc").kind!=Kind::number)unsupported("unbound wide select flag");
        e="("+scalar_condition()+" ? "+u(1)+" : "+u(op=="s_cmov_b64"?0:2)+")";
    }else if(op=="s_and_b64"||op=="s_or_b64"||op=="s_xor_b64"||op=="s_andn2_b64"||op=="s_orn2_b64"||
             op=="s_nand_b64"||op=="s_nor_b64"||op=="s_xnor_b64"||op=="s_not_b64"){
        if(op=="s_not_b64")e="~"+u(1);
        else {e="("+u(1)+(op.contains("and")?" & ":op.contains("xor")||op.contains("xnor")?" ^ ":" | ")+(op.contains("n2_")?"~":"")+u(2)+")";
            if(op.starts_with("s_n")||op.starts_with("s_xnor"))e="~"+e;}
        cc=true;
    }else if(op=="v_lshlrev_b64"||op=="v_lshrrev_b64"||op=="v_ashrrev_i64"||op=="s_lshl_b64"||
             op=="s_lshr_b64"||op=="s_ashr_i64"){
        auto v=u(vec?2:1);if(op.contains("ashr"))v="int64_t("+v+")";
        e="uint64_t("+v+(op.contains("lshl")?" << (":" >> (")+b(vec?1:2)+" & 63u))";cc=!vec;
    }else if(op=="s_bfm_b64")e="(((uint64_t(1)<<("+b(1)+"&63u))-1u)<<("+b(2)+"&63u))";
    else if(op=="s_bitset0_b64"||op=="s_bitset1_b64")e=u(0)+(op.contains("set0")?" & ~(uint64_t(1)<<(":" | (uint64_t(1)<<(")+b(1)+"&63u))";
    else if(op=="s_bitcmp0_b64"||op=="s_bitcmp1_b64"){
        write("scc","uint(("+u(0)+">>("+b(1)+"&63u))&1u)"+(op.contains("cmp0")?" ^ 1u":""),scalar);return true;
    }else if(op=="s_brev_b64"){auto p=wide(a[1]);e="rd_u64(reversebits("+p+").yx)";}
    else if(op=="s_bfe_i64"||op=="s_bfe_u64")e=std::format("rd_bfe64({},{},{})",u(1),b(2),op.ends_with("i64")?"true":"false"),cc=true;
    else if(op=="s_bcnt0_i32_b64"||op=="s_bcnt1_i32_b64"||op=="s_ff0_i32_b64"||op=="s_ff1_i32_b64"||
            op=="s_flbit_i32_b64"||op=="s_flbit_i32_i64"){
        auto w=wide(a[1]);if(op.contains("bcnt0")||op.contains("ff0"))w="(~"+w+")";
        w=temp("uint2",w);two=false;
        if(op.contains("bcnt"))e="countbits("+w+".x)+countbits("+w+".y)",cc=true;
        else if(op.contains("ff0")||op.contains("ff1"))e=std::format("({0}.x!=0u?firstbitlow({0}.x):{0}.y!=0u?32u+firstbitlow({0}.y):~0u)",w);
        else {if(op.ends_with("i64"))w=temp("uint2",std::format("asint({0}.y)<0?~{0}:{0}",w));
            e=std::format("({0}.y!=0u?rd_clz({0}.y):{0}.x!=0u?32u+rd_clz({0}.x):~0u)",w);}
    }else if(op=="v_cvt_f64_f32"||op=="v_cvt_f64_i32"||op=="v_cvt_f64_u32"){
        soft_double=true;e=op.ends_with("f32")?"sf_from_f32("+b(1)+")":"sf_from_int("+b(1)+","+(op.ends_with("i32")?"true":"false")+")";
    }else if(op=="v_cvt_f32_f64")e="sf_to_f32("+d(1)+")",two=false;
    else if(op=="v_cvt_i32_f64"||op=="v_cvt_u32_f64"){
        e=std::format("sf_to_int({},{})",d(1),op.contains("i32_")?"true":"false");two=false;
    }else if(op=="v_add_f64")e=std::format("sf_add({},{})",d(1),d(2));
    else if(op=="v_mul_f64")e=std::format("sf_muladd({},{},0,false)",d(1),d(2));
    else if(op=="v_fma_f64")e=std::format("sf_muladd({},{},{},true)",d(1),d(2),d(3));
    else if(op=="v_ldexp_f64")e=std::format("sf_ldexp({},asint({}))",d(1),b(2));
    else if(op=="v_frexp_mant_f64")e="sf_mant("+d(1)+")";
    else if(op=="v_frexp_exp_i32_f64")e="asuint(sf_exp("+d(1)+"))",two=false;
    else if(op=="v_rcp_f64"||op=="v_rsq_f64"||op=="v_sqrt_f64")e=op=="v_rsq_f64"?"sf_rcp(sf_sqrt("+d(1)+"))":
        (op=="v_rcp_f64"?"sf_rcp(":"sf_sqrt(")+d(1)+")";
    else if(op=="v_trunc_f64"||op=="v_floor_f64"||op=="v_ceil_f64"||op=="v_rndne_f64"||op=="v_fract_f64"){
        e=std::format("sf_round({},{}u)",d(1),op=="v_trunc_f64"?0:op=="v_floor_f64"||op=="v_fract_f64"?1:op=="v_ceil_f64"?2:3);
        if(op=="v_fract_f64")e="sf_add("+d(1)+","+e+" ^ SF_SIGN)";
    }else if(op=="v_min_f64" || op=="v_max_f64"){
        auto l=temp("uint64_t",d(1)),r=temp("uint64_t",d(2));bool min=op=="v_min_f64";
        e=std::format("(sf_mag({0})>SF_INF?{1}:sf_mag({1})>SF_INF?{0}:rd_fp_relation(rd_words({0}),rd_words({1}),0x7ff00000u)=={2}u?{0}:sf_mag({0})==0u && sf_mag({1})==0u?({0}{3}{1}):{1})",
            l,r,min?1:4,min?"|":"&");
    }
    else return false;
    auto dst=registers(a[0]);check_range(dst,vec?'v':'s',two?2:1);
    auto q=temp(two?"uint64_t":"uint",e);
    if(cc)write("scc","uint("+q+" != 0u)",scalar);
    auto v=vec?varying:scalar;
    if(two){auto w=temp("uint2","rd_words("+q+")");
        write(reg(dst.bank,dst.first),w+".x",v,vec);write(reg(dst.bank,dst.first+1),w+".y",v,vec);}
    else write(a[0],q,v,vec);
    return true;
}
bool Lower::permute_alu(const Instruction& i) {
    if(!virtual_wave)unsupported("DPP requires a guest wave");
    auto& a=i.args;bool cross=i.op.starts_with("v_permlane");Str which,tokens;
    bool bound=false,fi=false;unsigned rows=15,banks=15;
    if(cross){
        if(a.size()!=4)unsupported("permlane operand count");
        auto sel=std::format("(((lane&8u)!=0u?{}:{}) >> ((lane&7u)*4u)) & 15u",bits(a[3]),bits(a[2]));
        which=std::format("(((lane&~15u) ^ {}u) | ({}))",i.op=="v_permlanex16_b32"?16:0,sel);
        auto mod=View(i.modifiers);
        if(!mod.empty()){
            if(mod!="op_sel:[1,0]" && mod!="op_sel:[0,1]" && mod!="op_sel:[1,1]" && mod!="op_sel:[0,0]")unsupported("permlane modifier");
            fi=mod[8]=='1';bound=mod[10]=='1';
        }
    }else {
        std::istringstream in(i.modifiers);Str t;bool mapping=false;
        while(in>>t){
            if(t.starts_with("row_mask:")){rows=number(View(t).substr(9));continue;}
            if(t.starts_with("bank_mask:")){banks=number(View(t).substr(10));continue;}
            if(t=="bound_ctrl:1" || t=="bound_ctrl:0"){bound=t.back()=='1';continue;}
            if(t=="fi:1"||t=="fi:0"){fi=t.back()=='1';continue;}
            if(t=="clamp"||t=="mul:2"||t=="mul:4"||t=="div:2"){tokens+=(tokens.empty()?"":" ")+t;continue;}
            if(mapping)unsupported("multiple DPP lane controls");
            mapping=true;
            if(t.starts_with("dpp8:[") || t.starts_with("quad_perm:[")){
                bool eight=t.starts_with("dpp8:");unsigned n=eight?8:4,per=eight?3:2,mask=0;
                auto open=t.find('[');View v=View(t).substr(open+1);
                if(!v.ends_with(']'))unsupported("invalid DPP permutation");
                v.remove_suffix(1);
                for(unsigned k=0;k<n;++k){
                    auto pos=v.find(',');auto j=number(v.substr(0,pos));if(j>=n || (pos==v.npos)!=(k+1==n))unsupported("DPP permutation index");
                    mask|=j<<(per*k);if(pos!=v.npos)v.remove_prefix(pos+1);
                }
                which=std::format("((lane&~{0}u) | (({1}u>>((lane&{0}u)*{2}u))&{0}u))",n-1,mask,per);
            }else if(t=="row_mirror" || t=="row_half_mirror")which=t=="row_mirror"?"(lane^15u)":"(lane^7u)";
            else if(t.starts_with("row_")){
                auto pos=t.find(':');if(pos==t.npos)unsupported("unknown DPP row mapping");unsigned n=number(View(t).substr(pos+1));
                if(n>15)unsupported("DPP row distance");
                auto op=View(t).substr(0,pos);
                if(op=="row_shr")which=std::format("((lane&15u)>={0}u?lane-{0}u:~0u)",n);
                else if(op=="row_shl")which=std::format("((lane&15u)+{0}u<16u?lane+{0}u:~0u)",n);
                else if(op=="row_ror")which=std::format("((lane&~15u)|((lane-{}u)&15u))",n);
                else if(op=="row_share")which=std::format("((lane&~15u)|{}u)",n);
                else if(op=="row_xmask")which=std::format("(lane^{}u)",n);
                else unsupported("unknown DPP row mapping");
            }else unsupported("unknown DPP control "+t);
        }
        if(!mapping || rows>15 || banks>15)unsupported("invalid DPP execution mask");
    }
    if(a.size()<2 || !a[0].starts_with('v') || a[0].starts_with("vcc"))unsupported("DPP scalar destination");
    auto peer=temp("uint",which);
    // All physical lanes participate in the reads; only the destination write
    // obeys EXEC, row/bank masks and the out-of-bounds policy.
    auto active_peer=temp("uint","group_lane(uint(active),"+peer+",lane)");
    auto live=temp("bool",std::format("{} < LANES && ({}!=0u || {})",peer,active_peer,fi?"true":"false"));
    auto word=temp("uint","group_lane("+bits(a[1])+","+peer+",lane)");
    auto src=temp("uint",live+" ? "+word+" : 0u");set(src,varying);
    auto dest=a[0];vars.insert(dest);auto old=temp("uint",dest);
    if(cross)write(dest,src,varying,true);
    else{
        Instruction u=i;u.op.resize(u.op.size()-4);u.modifiers=tokens;u.args[1]=src;
        if(!numeric(u))unsupported("unimplemented DPP base "+u.op);
    }
    line(std::format("if(!active || !(({}u >> (lane/16u)) & 1u) || !(({}u >> ((lane/4u)&3u)) & 1u){}) {} = {};",
                     rows,banks,bound?"":" || !"+live,dest,old));
    representatives.erase(dest);return true;
}
bool Lower::relative_move(const Instruction& i){
    View op=i.op;
    bool swap=op=="v_swap_b32" || op=="v_swaprel_b32",vec=op.starts_with("v_");
    if(!swap && !op.starts_with("v_movrel") && !op.starts_with("s_movrel"))return false;
    if(i.args.size()!=2 || !i.modifiers.empty())unsupported("relative register operands");
    auto dst=registers(i.args[0]),src=registers(i.args[1]);unsigned words=op.ends_with("b64")?2:1;char bank=vec?'v':'s';
    check_range(dst,bank,words);check_range(src,bank,words);
    bool split=op.contains("_2_")||op=="v_swaprel_b32",rs=op.contains("rels")||split,rd=op.contains("reld")||op.contains("relsd")||split;
    auto c=value("m0_constant");bool fixed=c.kind==Kind::immediate;
    if(op=="v_swap_b32"||fixed){
        u64 x=src.first+(rs?u64(split?c.id&1023:c.id):0),y=dst.first+(rd?u64(split?(c.id>>16)&1023:c.id):0);
        if(x+words>(vec?256u:106u) || y+words>(vec?256u:106u))unsupported("relative register outside bank");
        struct Copy{Str to,expr;Value v;};std::vector<Copy> copies;
        auto copy=[&](unsigned from,unsigned to){
            auto srcname=reg(bank,from);auto v=value(srcname);Str expr;
            if(v.kind==Kind::number || v.kind==Kind::predicate || v.kind==Kind::mask_high || v.kind==Kind::code_lo || v.kind==Kind::code_hi)
                expr=temp("uint",srcname);
            else if(v.kind!=Kind::descriptor && v.kind!=Kind::table)unsupported("unbound relative register");
            copies.push_back({reg(bank,to),expr,v});
        };
        for(unsigned j=0;j<words;++j){copy(x+j,y+j);if(swap)copy(y+j,x+j);}
        for(auto& e:copies){if(e.expr.empty())set(e.to,e.v);else write(e.to,e.expr,e.v,vec);}
        return true;
    }
    if(auto rw=relative_windows.find(i.pc);rw!=relative_windows.end() && value("m0").kind==Kind::number){
        auto w=rw->second;auto bank="relbank"+std::to_string(w.base);auto width=std::to_string(w.width)+"u";
        if(w.store){
            auto val=bits(i.args[1]);line("if(active && m0<"+width+") "+bank+"[min(m0,"+width+"-1u)]="+val+";");
            for(unsigned k=0;k<w.width;++k){
                auto n=reg('v',w.base+k);auto t=peek(n);
                if(t.kind!=Kind::number && t.kind!=Kind::unknown && t.kind!=Kind::undef_word && t.kind!=Kind::masked_number)
                    unsupported("bounded relative destination overlaps nonnumeric word");
                set(n,varying);
            }
        }else{
            // Only the true arm of the *same* range predicate may observe
            // this read. An out-of-range register is not replaced by zero.
            vars.insert(i.args[0]);line("if(active && m0<"+width+") "+i.args[0]+"="+bank+"[min(m0,"+width+"-1u)];");
            set(i.args[0],{Kind::bounded_relative,w.condition});
        }
        return true;
    }
    if(!vec || !indexed_vgprs || value("m0").kind!=Kind::number)unsupported("dynamic relative move without numeric register bank");
    auto idx=[&](unsigned base,bool rel,bool dest){return temp("uint",std::to_string(base)+"u"+(!rel?"":split?
        dest?" + ((m0>>16u)&1023u)":" + (m0&1023u)":" + m0"));};
    auto x=idx(src.first,rs,false),y=idx(dst.first,rd,true);
    auto v=temp("uint",x+"<256u ? vgpr_file[min("+x+",255u)] : 0u");
    auto old=swap?temp("uint",y+"<256u ? vgpr_file[min("+y+",255u)] : 0u"):"";
    line("if(active && "+y+"<256u)vgpr_file[min("+y+",255u)]="+v+";");
    if(swap)line("if(active && "+x+"<256u)vgpr_file[min("+x+",255u)]="+old+";");
    for(unsigned j=0;j<256;++j)if(j==dst.first || (j>=dst.first && rd) || (swap && j>=src.first)){
        auto n=reg('v',j);auto v=peek(n);set(n,v.kind==Kind::unknown||v.kind==Kind::undef_word||v.kind==Kind::number?varying:Value{});
    }
    return true;
}
bool Lower::subword_buffer(const Instruction& i){
    View op=i.op;bool load=op.starts_with("buffer_load_"),storeop=op.starts_with("buffer_store_");
    if(!load && !storeop)return false;
    op.remove_prefix(load?12:13);
    bool format=op.starts_with("format_d16_"),high=false,partial=false,sign=false;
    unsigned width=0,n=1;
    if(format){
        op.remove_prefix(11);
        if(op.starts_with("hi_")){high=true;op.remove_prefix(3);}
        if(op.empty()||!xyzw.starts_with(op)|| (high && op!="x"))unsupported("packed buffer component mask");
        n=unsigned(op.size());
    }else{
        high=op.ends_with("_d16_hi");partial=high || op.ends_with("_d16");
        if(partial)op.remove_suffix(high?7:4);
        width=op=="byte"||op=="sbyte"||op=="ubyte"?8:op=="short"||op=="sshort"||op=="ushort"?16:0;
        if(!width)return false;
        sign=op=="sbyte"||op=="sshort";
    }
    auto& a=i.args;if(a.size()!=4)unsupported("subword MUBUF operands");
    auto id=descriptor(a[2]);const auto& r=p.resources[id];
    auto d=registers(a[0]);check_range(d,'v',format?(n+1)/2:1);
    bool idx=false,off=false;unsigned imm=0;std::istringstream m(i.modifiers);Str t;
    while(m>>t){if(t=="idxen")idx=true;else if(t=="offen")off=true;
        else if(t.starts_with("offset:"))imm=number(View(t).substr(7));else unsupported("subword MUBUF modifier");}
    auto byte=bits(a[3])+" + "+std::to_string(imm)+"u";Str index="0u";
    if(idx||off){
        auto v=registers(a[1]);check_range(v,'v',unsigned(idx)+unsigned(off));
        if(idx)index=bits(reg('v',v.first));
        if(off)byte+=" + "+bits(reg('v',v.first+unsigned(idx)));
    }else if(a[1]!="off")unsupported("subword buffer address");
    if(format){
        if(r.kind!=10 || (r.type!=6 && !(load&&r.type==3)) || off || imm || a[3]!="0")
            unsupported("packed typed buffer layout");
        half_images.insert(id);view_width(id,n,!load);auto res=std::format("res{}[{}]",id,index);
        if(load){
            if(masked)line("if(active){");
            auto words=temp("uint"+suffix(n),"asuint("+res+"."+Str(xyzw.substr(0,n))+")");
            auto half=[&](unsigned j){return std::format("PACK16_{}({}{})",id,words,n==1?"":Str(".")+xyzw[j]);};
            for(unsigned j=0;j<d.count;++j){
                Str dst=reg('v',d.first+j),val=half(j*2);
                if(high){vars.insert(dst);val="(("+dst+" & 65535u) | ("+val+" <<16u))";}
                else if(j*2+1<n)val+=" | ("+half(j*2+1)+" <<16u)";
                write(dst,val,varying);
            }
            if(masked)line("}");
        }else{
            Str val="uint"+suffix(n)+"(";
            for(unsigned j=0;j<n;++j)val+=std::format("{}UNPACK16_{}(({}>>{}u)&65535u)",j?",":"",id,bits(reg('v',d.first+j/2)),(high || j%2)?16:0);
            store(std::format("{} = BITS{}({}))",res,id,val));
        }
        return true;
    }
    bool raw=r.kind==11 && (r.type==7 || (load && r.type==4));
    bool str=r.kind==12 && r.stride && !(r.stride%4) && (r.type==8 || (load && r.type==5));
    if(!raw && !str)unsupported("untyped subword access requires raw or structured resource");
    if(idx && !r.stride)unsupported("unknown MUBUF index stride");
    if(str && (off || a[3]!="0" || imm+width/8>r.stride))unsupported("subword offset outside a structured element");
    auto b=temp("uint","("+byte+")"+(idx?std::format("+uint({})*{}u",index,r.stride):""));
    auto addr=raw?"("+b+"&~3u)":std::format("res{}[{} / {}u].w[({} % {}u)>>2u]",id,b,r.stride,b,r.stride);
    auto shift="(("+b+"&3u)*8u)",dst=reg('v',d.first);
    if(load){
        if(masked)line("if(active){");
        auto word=raw?std::format("res{}.Load({})",id,addr):addr;
        auto val=std::format("(({} >> {}) & {}u)",word,shift,(1u<<width)-1u);
        if(sign)val=std::format("asuint(asint({} << {}u) >> {}u)",val,32-width,32-width);
        if(partial){vars.insert(dst);val=high?"(("+dst+"&65535u) | ("+val+"<<16u))":"(("+dst+"&0xffff0000u) | ("+val+"&65535u))";}
        write(dst,val,varying);if(masked)line("}");
    }else{
        auto mask=std::to_string((1u<<width)-1)+"u";
        auto val="((("+bits(dst)+(high?">>16u)":" )")+"&"+mask+")<<"+shift+")";
        atomic32("mskor",addr,raw?std::format("res{}.",id):"",~0u,"("+mask+"<<"+shift+")",val,"");
    }
    return true;
}
