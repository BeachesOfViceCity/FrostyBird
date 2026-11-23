"""Prepare new PSO recipes using observed state and exact, compatible original material shaders."""
import collections
import hashlib
import struct
from pathlib import Path

from shader_cache import Request,atomic_write,read_bounded


def split_graphics(b):
    """Decode the pointer-free XSP1 format written in guest/pipeline_prep.cpp."""
    at=0
    def take(n):
        nonlocal at
        if n>len(b)-at:raise ValueError("truncated XSP1 state")
        old=at;at+=n;return b[old:at]
    def u32():return struct.unpack("<I",take(4))[0]
    def blob():
        n=u32()
        if n>16<<20:raise ValueError("oversized XSP1 blob")
        return take(n)
    if u32()!=0x31505358 or u32()!=2:return None
    blob();take(4+5*4+8) # root, depth bounds, scalar fields, SampleDesc
    nr=u32()
    if nr>8:raise ValueError("invalid target count")
    take(nr*4+4+8+8*37+44+50) # RTVs/DSV, blend (without padding), raster, depth/stencil
    n=u32()
    if n>32:raise ValueError("invalid vertex layout")
    for _ in range(n):blob();take(24)
    vs=blob();start=at;ps=blob();end=at;blob()
    if at!=len(b):raise ValueError("extra graphics state")
    return b[:start],vs,ps,b[end:]


def schema(raw):
    if len(raw)<32 or raw[:4]!=b"DXBC":return None
    n=struct.unpack_from("<I",raw,28)[0]
    if n>(len(raw)-32)//4:return None
    parts={}
    for at, in struct.iter_unpack("<I",raw[32:32+4*n]):
        if at+8>len(raw):return None
        size=struct.unpack_from("<I",raw,at+4)[0]
        if size>len(raw)-at-8:return None
        if raw[at:at+4] in (b"ISG1",b"OSG1",b"PSV0"):parts[raw[at:at+4]]=raw[at:at+8+size]
    return hashlib.sha256(b"".join(parts[k] for k in (b"ISG1",b"OSG1",b"PSV0"))).digest() if len(parts)==3 else None


def prepare(store,archive):
    root=store.root/"pipeline-recipes-v1";out=root/"predicted"
    if not root.is_dir():return {"templates":0,"new_plans":0,"new_hints":0}
    models={};hints=collections.defaultdict(list);layout={}
    def slots(data,wire):
        if not data:return ()
        req=Request.make(data,wire);key=(req.digest,wire)
        if key not in layout:
            packet=store.lookup(req,store.initial)
            layout[key]=None if packet is None else struct.unpack_from("<"+"I"*struct.unpack_from("<I",packet)[0],packet,8)
        return layout[key]
    for hp in root.glob("*.hint"):
        words=hp.name.split(".")
        if len(words)!=3:continue
        data=read_bounded(hp,512)
        if hashlib.sha256(data).hexdigest()==words[1]:hints[words[0]].append(data)
    for plan in root.glob("*.plan"):
        b=read_bounded(plan,64<<20)
        if hashlib.sha256(b).hexdigest()!=plan.stem:continue
        try:state=split_graphics(b)
        except ValueError:continue
        if state is None:continue
        pre,vs,ps,post=state;s=schema(ps)
        if s is None:continue
        key=hashlib.sha256(pre+b"\0\0\0\0"+post).digest()
        m=models.setdefault(key,{"pre":pre,"post":post,"vs":vs,"seen":set(),"forms":collections.defaultdict(set)})
        m["seen"].add(hashlib.sha256(ps).digest())
        known=m["forms"][s]
        v,p=slots(vs,1),slots(ps,0)
        if v is not None and p is not None:
            for h in hints[plan.stem]:
                if len(h)==len(v)+len(p):known.add((h[:len(v)],p,h[len(v):]))
    by_schema=collections.defaultdict(list);nt=0
    for m in models.values():
        if len(m["seen"])<3:continue
        nt+=1
        for s in m["forms"]:by_schema[s].append(m)
    if not nt:return {"templates":0,"new_plans":0,"new_hints":0}
    out.mkdir(exist_ok=True);count=n_hints=0
    for req in archive:
        if req.wire!=0:continue
        linkage=schema(req.raw)
        if linkage not in by_schema:continue
        dg=bytes.fromhex(req.digest);p=slots(req.raw,0)
        if p is None:continue
        for m in by_schema[linkage]:
            if dg in m["seen"]:continue
            m["seen"].add(dg)
            recipe=m["pre"]+struct.pack("<I",len(req.raw))+req.raw+m["post"];key=hashlib.sha256(recipe).hexdigest()
            path=out/(key+".plan")
            if not path.is_file():atomic_write(path,recipe);count+=1
            for vs,old,ps in m["forms"][linkage]:
                if p!=old:
                    # Only uniform numeric request bytes scale to a different
                    # slot count. Never predict dimension or sampler fields.
                    if not ps or len(set(ps))!=1 or ps[0] not in (1,2) or any(x&~0x37f for x in old+p):continue
                    ps=ps[:1]*len(p)
                if ps and store.lookup(Request.make(req.raw,0,ps),store.initial) is None:continue
                v=vs+ps
                if not any(v):continue
                hp=out/(key+"."+hashlib.sha256(v).hexdigest()+".hint")
                if not hp.is_file():atomic_write(hp,v);n_hints+=1
    return {"templates":nt,"new_plans":count,"new_hints":n_hints}
