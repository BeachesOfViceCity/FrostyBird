"""Precompile representative native graphics recipes from the title's cooked material maps."""
from collections import Counter,defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import struct
import subprocess
import time

from pipeline_families import split_graphics
from prewarm_shaders import archive_requests
from shader_cache import CompileError,Request,ShaderCache,atomic_write
from stage_families import declared,short_key

def signature(raw,tag,inputs=False):
    if raw[:4]!=b"DXBC":return None
    n=struct.unpack_from("<I",raw,28)[0]
    if n>(len(raw)-32)//4:return None
    for at, in struct.iter_unpack("<I",raw[32:32+4*n]):
        if raw[at:at+4]!=tag:continue
        size=struct.unpack_from("<I",raw,at+4)[0];d=raw[at+8:at+8+size]
        if len(d)<8:return None
        count,off=struct.unpack_from("<II",d)
        if count>128 or off+32*count>len(d):return None
        result=[]
        for stream,name,index,system,ty,reg,mask,used,_,prec in struct.iter_unpack("<6IBBHI",d[off:off+32*count]):
            end=d.find(b"\0",name)
            if end<name or stream:return None
            result.append((d[name:end].upper(),index,system,ty,reg,mask,prec))
        return tuple(sorted(result))
    return None

def bindings(raw):
    decl=declared(raw)
    if decl is None:return None
    b=set()
    for kind,space,lo,hi,_,_ in decl:
        if not 1<=kind<=9 or hi<lo or hi-lo>256:return None
        letter="s" if kind==1 else "b" if kind==2 else "t" if kind<=5 else "u"
        b.update((letter,space,i) for i in range(lo,hi+1))
    return b

def provides(vout,pin):
    have={(name,index):(ty,reg,mask,prec) for name,index,system,ty,reg,mask,prec in vout}
    for name,index,system,ty,reg,mask,prec in pin:
        out=have.get((name,index))
        if out is None:
            if system not in (1,7,9,10,66):return False
        elif out[0]!=ty or out[1]!=reg or out[2]&mask!=mask or out[3]!=prec:return False
    return True

def prepare(tool,indexer,cache,pak,jobs,limit,report=None,*,before=None,preview=False):
    start=time.monotonic();store=ShaderCache(tool,cache);root=store.root/"pipeline-recipes-v1"
    by_vs=defaultdict(list);models={};root_slots=defaultdict(set);known=set();stages={};formats={};hints=defaultdict(set)
    def stage(raw,wire):
        dg=hashlib.sha256(raw).digest()
        if dg not in stages:
            req=Request.make(raw,wire);pkt=store.lookup(req,store.initial)
            slots=None if pkt is None else struct.unpack_from("<"+"I"*struct.unpack_from("<I",pkt)[0],pkt,8)
            stages[dg]=(short_key(raw),slots)
        return stages[dg]
    for path in root.glob("*.hint"):
        parts=path.name.split(".")
        if len(parts)==3 and (before is None or path.stat().st_mtime<before) and path.stat().st_size<=512:
            h=path.read_bytes()
            if hashlib.sha256(h).hexdigest()==parts[1]:hints[parts[0]].add(h)
    for path in root.glob("*.plan"):
        if before is not None and path.stat().st_mtime>=before:continue
        b=path.read_bytes()
        if hashlib.sha256(b).hexdigest()!=path.stem:continue
        g=split_graphics(b)
        if not g:continue
        pre,vs,ps,post=g
        if not vs or not ps or post!=b"\0"*4:continue
        rid=hashlib.sha256(pre[:12+struct.unpack_from("<I",pre,8)[0]]).digest()
        key=hashlib.sha256(pre+post).digest();v,vs_slots=stage(vs,1);p,ps_slots=stage(ps,0)
        br=bindings(ps);f=signature(ps,b"OSG1");vo=signature(vs,b"OSG1")
        if br is None or f is None or vo is None:continue
        known.add(p);root_slots[rid].update(br)
        if key not in models:
            models[key]={"rid":rid,"pre":pre,"post":post,"vout":vo,"forms":defaultdict(set),"patterns":defaultdict(Counter),"uniform":defaultdict(Counter)}
            by_vs[v].append(key)
        m=models[key];m["forms"][f].add(p);formats[p]=f
        if vs_slots is not None and ps_slots is not None:
            for h in hints[path.stem]:
                if len(h)!=len(vs_slots)+len(ps_slots):continue
                a,z=h[:len(vs_slots)],h[len(vs_slots):];m["patterns"][f,ps_slots][a,z]+=1
                if z and len(set(z))==1 and z[0] in (1,2) and all(not (u&~0x37f) for u in ps_slots):m["uniform"][f][a,z[0]]+=1
    # Only pairs explicitly co-resident in a shader map, using a vertex
    # program for which the game has supplied at least one complete state.
    rows=subprocess.check_output([indexer,pak],text=True)
    possible=defaultdict(dict)
    for line in rows.splitlines():
        _,*words=line.split();vs=set();ps=set()
        for w in words:
            stage_id,k=w.split(":");(vs if stage_id=="1" else ps).add(int(k,16))
        seen=ps&known
        for v in vs&by_vs.keys():
            for key in by_vs[v]:
                m=models[key]
                for form,prev in m["forms"].items():
                    same=len(seen&prev)
                    score=32*min(5,same)+2*min(8,len(seen))+min(10,len(prev))
                    for p in ps-prev:
                        # A PS already present under some other state can
                        # still need a new exact native pipeline or RT swizzle.
                        if p in formats and formats[p]!=form:continue
                        k=(key,form);possible[p][k]=max(score,possible[p].get(k,0))
    candidates=[]
    for p,req in archive_requests(store,pak,None,"0",lambda st,k:st==0 and k in possible,with_keys=True):
        form=signature(req.raw,b"OSG1");pin=signature(req.raw,b"ISG1",True);decl=bindings(req.raw)
        if form is None or pin is None or decl is None:continue
        matches=[]
        for (key,wanted),score in possible[p].items():
            m=models[key]
            if form==wanted and decl<=root_slots[m["rid"]] and provides(m["vout"],pin):matches.append((score,key))
        if matches:
            score,key=max(matches)
            # A previously unseen long material costs far more to translate
            # than a small pass. Size ranks *speculation* only, never lookup.
            candidates.append((score*(1+min(12,len(req.raw)//4096)),p,key,form,req))
    candidates.sort(key=lambda x:(-x[0],x[1],x[2]))
    # Representative recipes train the driver off line. They do not compete
    # with observed, exact states for the live player's bounded replay pool.
    selected=candidates[:limit];pred=root/"training";pred.mkdir(exist_ok=True)
    def make(row):
        score,p,key,form,r=row;m=models[key];b=m["pre"]+struct.pack("<I",len(r.raw))+r.raw+m["post"]
        dg=hashlib.sha256(b).hexdigest()
        if preview:return dg,0,0,0,None
        try:
            pkt=store.resolve(r);slots=struct.unpack_from("<"+"I"*struct.unpack_from("<I",pkt)[0],pkt,8)
            patterns=Counter(m["patterns"].get((form,slots),{}))
            if slots and all(not (k&~0x37f) for k in slots):
                for (v,t),freq in m["uniform"].get(form,{}).items():patterns[v,bytes([t])*len(slots)]+=freq
            typed=0
            for (v,t),_ in patterns.most_common(2):
                if not any(v+t):continue
                store.resolve(Request.make(r.raw,0,t));h=v+t
                hp=pred/(dg+"."+hashlib.sha256(h).hexdigest()+".hint")
                if not (root/hp.name).is_file() and not hp.exists():atomic_write(hp,h)
                typed+=1
            path=pred/(dg+".plan");new=not (root/path.name).is_file() and not path.is_file()
            if new:atomic_write(path,b)
            return dg,new,typed,0,None
        except CompileError as e:return dg,0,0,int(e.code==3),None if e.code==3 else str(e)
    with ThreadPoolExecutor(jobs) as pool:results=list(pool.map(make,selected))
    data=dict(cooked_maps=len(rows.splitlines()),templates=len(models),viable=len(candidates),selected=len(selected),
              new_plans=sum(r[1] for r in results),typed=sum(r[2] for r in results),unsupported=sum(r[3] for r in results),
              errors=[x[4] for x in results if x[4]],elapsed_s=round(time.monotonic()-start,3),**store.summary())
    if preview:
        data["selected_plans"]=[x[0] for x in results]
        data["selected_pixels"]=[f"{p:016x}" for _,p,_,_,_ in selected]
    if report:atomic_write(report,(json.dumps(data,indent=2)+"\n").encode())
    print("Cooked pair prewarm:",{k:v for k,v in data.items() if k not in ("selected_plans","selected_pixels","errors")},len(data["errors"]),"errors",flush=True)
    return data
