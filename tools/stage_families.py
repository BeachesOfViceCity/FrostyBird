"""Prewarm the title's cooked Global permutations and vertex linkage siblings.

The index limits discovery. Every shader and PSO served live must still
match the exact original bytecode, ABI, root, state, and specialization.
"""
import collections
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import json
import struct
import time

from prewarm_shaders import archive_requests,pixel_linkage
from shader_cache import CompileError,Request,ShaderCache,atomic_write

def short_key(raw):
    k=0xcbf29ce484222325
    for c in raw:k=(k*0x100000001b3&0xffffffffffffffff)^c
    return k

def declared(raw):
    n=struct.unpack_from("<I",raw,28)[0]
    for at, in struct.iter_unpack("<I",raw[32:32+4*n]):
        if raw[at:at+4]!=b"PSV0":continue
        length=struct.unpack_from("<I",raw,at+4)[0];v=raw[at+8:at+8+length]
        if len(v)<8:return None
        off=4+struct.unpack_from("<I",v)[0]
        if off+4>len(v):return None
        count=struct.unpack_from("<I",v,off)[0]
        if not count:return frozenset()
        if off+8+24*count>len(v) or struct.unpack_from("<I",v,off+4)[0]!=24:return None
        return frozenset(struct.iter_unpack("<6I",v[off+8:off+8+24*count]))
    return None

def prepare(tool,cache,pak,index,caps,jobs,report=None):
    if not 0<=caps<=15 or not 1<=jobs<=32:raise ValueError("invalid device mask or job count")
    start=time.monotonic();store=ShaderCache(tool,cache)
    names=collections.defaultdict(set)
    with index.open() as f:
        for r in csv.DictReader(f,delimiter="\t"):names[int(r["key"],16)].add(r["type_hash"])
    models=collections.defaultdict(lambda:(collections.defaultdict(collections.Counter),collections.Counter()))
    keys={};slotmaps={}
    def slots(pkt):return struct.unpack_from("<"+"I"*struct.unpack_from("<I",pkt)[0],pkt,8)
    def groups(stage,key,raw):
        for name in names.get(key,()):yield (stage,"global",name)
        if stage==1:
            linkage=pixel_linkage(raw)
            if linkage:yield (stage,"vertex",linkage)
    for p in store.recipes.iterdir():
        if p.name.startswith("."):continue
        rawid,wire,_=p.name.split(".");stage=int(wire)&255
        if stage not in (1,2,5):continue
        r=store.recorded(p.name);w=stage|(caps<<8) if stage==5 else stage
        if rawid not in keys:keys[rawid]=short_key(r.raw)
        if (rawid,w) not in slotmaps:
            pkt=store.lookup(Request.make(r.raw,w),store.initial);slotmaps[rawid,w]=None if pkt is None else slots(pkt)
        v=slotmaps[rawid,w]
        for g in groups(stage,keys[rawid],r.raw):
            exact,uniform=models[g]
            if v is not None and len(r.params)==len(v) and r.params:
                exact[v][r.params]+=1
                if all(not (n&~0x37f) for n in v) and len(set(r.params))==1 and r.params[0] in (1,2):uniform[r.params[0]]+=1
    root=store.root/"pipeline-recipes-v1";recipes=collections.defaultdict(dict)
    if root.is_dir():
        for path in root.glob("*.plan"):
            b=path.read_bytes()
            if len(b)<24 or struct.unpack_from("<II",b)!=(0x31505358,1):continue
            off=12+struct.unpack_from("<I",b,8)[0]+8
            if off+4>len(b) or struct.unpack_from("<I",b,off)[0]!=len(b)-off-4 or hashlib.sha256(b).hexdigest()!=path.stem:continue
            raw=b[off+4:];d=declared(raw)
            if d is None:continue
            for g in groups(5,short_key(raw),raw):recipes[g].setdefault(b[:off],set()).add(d)
    candidates={};matched={}
    for k,r in archive_requests(store,pak,None,str(caps),lambda stage,k:stage in (1,2,5),with_keys=True):
        gs=[g for g in groups(r.wire&255,k,r.raw) if g in models]
        if gs:candidates[r.name]=r;matched[r.name]=gs
    def warm(r):
        try:
            v=slots(store.resolve(r));patterns=collections.Counter()
            for g in matched[r.name]:
                exact,uniform=models[g];patterns.update(exact.get(v,{}))
                if v and all(not (x&~0x37f) for x in v):
                    for t,n in uniform.items():patterns[bytes([t])*len(v)]+=n
            variants=[params for params,_ in patterns.most_common(16)]
            for params in variants:store.resolve(Request.make(r.raw,r.wire,params))
            created=0
            if r.wire&255==5 and recipes:
                decl=declared(r.raw);pred=root/"predicted";pred.mkdir(exist_ok=True)
                for g in matched[r.name]:
                    for header,seen in recipes[g].items():
                        if decl is None or not any(decl<=covered for covered in seen):continue
                        data=header+struct.pack("<I",len(r.raw))+r.raw;key=hashlib.sha256(data).hexdigest()
                        path=pred/(key+".plan")
                        if not (root/path.name).is_file() and not path.is_file():atomic_write(path,data);created+=1
                        for types in variants:
                            hp=pred/(key+"."+hashlib.sha256(types).hexdigest()+".hint")
                            if not (root/hp.name).is_file() and not hp.is_file():atomic_write(hp,types)
            return len(variants),created,0,None
        except CompileError as e:return 0,0,int(e.code==3),None if e.code==3 else (r.name,str(e))
    variants=predicted=unsupported=0;errors=[]
    with ThreadPoolExecutor(jobs) as pool:
        for n,pp,skip,err in pool.map(warm,candidates.values()):
            variants+=n;predicted+=pp;unsupported+=skip
            if err:errors.append(err)
    result=dict(mode="stage families",caps=caps,families=len(models),candidates=len(candidates),typed=variants,
                new_plans=predicted,unsupported_sources=unsupported,errors=errors,elapsed_seconds=round(time.monotonic()-start,3),**store.summary())
    if report:atomic_write(report,(json.dumps(result,indent=2)+"\n").encode())
    print("Stage prewarm:",json.dumps({k:v for k,v in result.items() if k!="errors"}),f"({len(errors)} errors)",flush=True)
    return result
