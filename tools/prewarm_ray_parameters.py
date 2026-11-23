#!/usr/bin/env python3
"""Precompile checked raygen variants which share actual parameter fields with a captured dispatch."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import os
from pathlib import Path
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]
TOOL=ROOT/"build/host/frosty-shader"
def rows(p):
    with p.open() as f:return list(csv.DictReader(f,delimiter="\t"))

def direct_parameters(index,wanted):
    """FShader's frozen resource parameters are (member byte offset, register, member kind)."""
    maps={}
    for r in rows(index/"global-maps.tsv"):
        mh,ids=maps.setdefault(int(r["section"]),(bytes.fromhex(r["map_hash"]),[]))
        if int(r["local_index"])!=len(ids):raise ValueError("inconsistent local index")
        ids.append(int(r["global_index"]))
    b=(index/"global-cache.bin").read_bytes();u=lambda n:struct.unpack_from("<I",b,n)[0]
    if u(0)!=len(maps):raise ValueError("section count mismatch")
    at=4;found={}
    for section in range(len(maps)):
        mh,ids=maps[section];align,ver,n=struct.unpack_from("<III",b,at)
        if (align,ver)!=(8,5):raise ValueError("unknown frozen map")
        start=at+12;end=start+n
        def ptr(addr,size):
            # The relocation is a signed 32-bit displacement; the next
            # dword is not its sign extension on forward frozen pointers.
            delta=struct.unpack_from("<i",b,addr)[0];target=addr+(delta>>1)
            if not delta&1 or target<start or target+size>end:raise ValueError("invalid frozen relative pointer")
            return target
        count=u(start+64)
        if set(ids)&wanted:
            table=ptr(start+56,8*count);types=ptr(start+24,8*count)
            for j in range(count):
                sh=ptr(table+8*j,0xc8);idx=ids[u(sh+0xc4)]
                if idx not in wanted:continue
                cls=struct.unpack_from("<Q",b,types+8*j)[0];num=u(sh+0x18)
                if num!=u(sh+0x1c) or num>256:raise ValueError("unknown resource array size")
                arr=ptr(sh+0x10,4*num) if num else 0
                records=tuple((("field",cls,member,kind),reg) for member,reg,kind in struct.iter_unpack("<HBB",b[arr:arr+4*num]))
                if found.setdefault(idx,records)!=records:raise ValueError("aliased shader has different parameter structure")
        pos=b.find(mh,end)
        if pos<end:raise ValueError("missing section's actual archive hash")
        at=pos+len(mh)
    return found

def uniform_resources(raw):
    """Recover table bindings by UB layout hash and resource member, independent of its register."""
    end=raw.index(b"SPTR");bits,=struct.unpack_from("<I",raw);at=4;arrays=[]
    for _ in range(5):
        n,=struct.unpack_from("<I",raw,at);at+=4
        if n>4096 or at+4*n>end:raise ValueError("unknown resource table")
        arrays.append(struct.unpack_from(f"<{n}I",raw,at));at+=4*n
    hashes=arrays[3];out=[]
    for kind,cls in ((0,"t"),(1,"s"),(2,"u"),(4,"t")):
        a=arrays[kind]
        if not a:continue
        if len(a)<len(hashes)+1 or a[-1]!=0xffffffff:raise ValueError("invalid table terminator")
        for j,token in enumerate(a[len(hashes):-1],len(hashes)):
            ub=token>>24;member=(token>>8)&0xffff;reg=token&255
            if ub>=len(hashes) or not bits&(1<<ub) or not 0<a[ub]<=j or a[a[ub]]>>24!=ub:
                raise ValueError("resource entry disagrees with UB slot")
            out.append((("uniform",hashes[ub],member,kind),cls,reg))
    return out

def prepare(pak,reference,capture,out,work,jobs):
    idx=ROOT/"build/gameplay-index";all_rows=rows(idx/"types.tsv")
    ref_index=int(reference);ref=next(r for r in all_rows if int(r["global_index"])==ref_index)
    group={int(r["global_index"]):r for r in all_rows if r["type_hash"]==ref["type_hash"]}
    archive={int(r["index"]):r for r in rows(ROOT/"build/hwrt-package/ray-sha-index.tsv") if r["library"]=="Global"}
    direct=direct_parameters(idx,set(group));work.mkdir(exist_ok=True,parents=True)
    def inspect(n):
        d=work/str(n);d.mkdir(exist_ok=True)
        subprocess.run([TOOL,"dump",pak,"Global",str(n),d],stdout=subprocess.DEVNULL,check=True)
        tables=uniform_resources((d/f"Global-{n}.uecode").read_bytes())
        s=subprocess.check_output([TOOL,"rt-bindings",pak,"Global",str(n)],stderr=subprocess.PIPE,text=True)
        shapes={parts[1]:" ".join(parts[2:-1]) for line in s.splitlines()[1:] if (parts:=line.split())[0]=="space1" and parts[2]!="CBV"}
        return d,tables,shapes
    _,uniform,ref_shapes=inspect(ref_index);key=archive[ref_index]["collection_key"]
    log_plan=(out/(key+".checked.plan")).read_text().splitlines()
    if (out/(key+".xsrt")).read_bytes()!=(capture/(key+".xsrt")).read_bytes():raise ValueError("reference is not the supplied shader")
    words,route,field,value,as_slot,_=map(int,log_plan[0].split())
    metadata={}
    for l in log_plan[1:]:
        reg,dim,ty=l.split();v=int(reg);metadata[("u" if v&256 else "t")+str(v&127)]=(int(dim),ty)
    metadata[f"t{as_slot}"]=(11,"TLAS")
    identities={};used=set()
    for identity,cls,reg in uniform:
        name=cls+str(reg)
        if name in metadata:identities[identity]=(cls,*metadata[name],ref_shapes[name]);used.add(name)
    for identity,reg in direct[ref_index]:
        choices=[c for c in ("t","u") if c+str(reg) in metadata and c+str(reg) not in used]
        if len(choices)!=1:raise ValueError("ambiguous reference field")
        cls=choices[0];name=cls+str(reg);identities[identity]=(cls,*metadata[name],ref_shapes[name]);used.add(name)
    if used!=metadata.keys():raise ValueError("reference contains unidentified resource fields")
    def one(n):
        k=archive[n]["collection_key"];prefix=(n,group[n]["permutation"],k)
        if (out/(k+".checked.plan")).exists():return (*prefix,"already prepared","")
        try:
            d,ub,shapes=inspect(n);bound={}
            for identity,reg in (list(direct[n])+[(identity,reg) for identity,_,reg in ub]):
                if identity not in identities:continue
                cls,dim,ty,shape=identities[identity];name=cls+str(reg)
                if name in shapes and shapes[name]==shape:
                    if name in bound:raise ValueError("duplicate bound resource")
                    bound[name]=(dim,ty)
            if set(shapes)!=bound.keys():return (*prefix,"needs live fields",",".join(sorted(set(shapes)-bound.keys())))
            as_regs=[int(n[1:]) for n,(_,t) in bound.items() if t=="TLAS"]
            if len(as_regs)!=1:raise ValueError("expected one AS")
            layout={r:t for r,(_,t) in bound.items() if t!="TLAS"}
            args=[TOOL,"rt-lift-checked",pak,"Global",str(n),d,str(words),",".join(f"{r}={t}" for r,t in sorted(layout.items())),str(route),str(field),str(value)]
            subprocess.run(args,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True,check=True)
            if (d/(k+".xsrt")).read_bytes()!=(capture/(k+".xsrt")).read_bytes():raise ValueError("different original collection")
            req=[f"{int(r[1:])+(256 if r.startswith('u') else 0)} {bound[r][0]} {t}" for r,t in sorted(layout.items())]
            (d/(k+".checked.plan")).write_text(" ".join(map(str,(words,route,field,value,as_regs[0],len(req))))+"\n"+"\n".join(req)+"\n")
            for ext in (".xsrt",".rt.abi",".checked.dxil",".checked.plan"):os.replace(d/(k+ext),out/(k+ext))
            return (*prefix,"prepared","")
        except subprocess.CalledProcessError as e:return (*prefix,"unmapped",(e.stderr or str(e)).strip().splitlines()[0])
        except (OSError,ValueError) as e:return (*prefix,"unavailable",str(e))
    with ThreadPoolExecutor(jobs) as pool:results=list(pool.map(one,sorted(group)))
    with (work/"bindings.tsv").open("w") as f:
        w=csv.writer(f,delimiter="\t");w.writerow(("index","permutation","collection","status","detail"));w.writerows(results)
    from collections import Counter
    print(dict(Counter(r[3] for r in results)))
if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("pak",type=Path);p.add_argument("reference_index",type=int);p.add_argument("capture",type=Path);p.add_argument("out",type=Path);p.add_argument("work",type=Path)
    p.add_argument("--jobs",type=int,default=3);a=p.parse_args();prepare(a.pak,a.reference_index,a.capture,a.out,a.work,a.jobs)
