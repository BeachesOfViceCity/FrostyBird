"""Prepare original cooked graphics stages, including common exact typed requests."""
from collections import Counter,defaultdict
from concurrent.futures import FIRST_COMPLETED,ThreadPoolExecutor,wait
import json
import struct
import time

from prewarm_shaders import archive_requests
from shader_cache import CompileError,Request,ShaderCache,atomic_write

def prepare(tool,cache,pak,jobs,report,compute_caps=None):
    start=time.monotonic();store=ShaderCache(tool,cache);patterns=defaultdict(Counter)
    if not 1<=jobs<=32:raise ValueError("expected 1–32 compiler jobs")
    if compute_caps is not None and not 0<=compute_caps<=15:raise ValueError("expected the target adapter's compute mask")
    def slots(pkt):return struct.unpack_from("<"+"I"*struct.unpack_from("<I",pkt)[0],pkt,8)
    for f in store.recipes.iterdir():
        if f.name.startswith(".") or not f.stat().st_size:continue
        st=int(f.name.split(".")[1])&255
        if st not in ((0,1,2) if compute_caps is None else (5,)):continue
        wire=st if compute_caps is None else 5|(compute_caps<<8)
        r=store.recorded(f.name);pkt=store.lookup(Request.make(r.raw,wire),store.initial)
        if pkt is not None:
            v=slots(pkt)
            if len(v)==len(r.params):patterns[wire,v][r.params]+=1
    unsupported=store.initial.directory/"unsupported";unsupported.mkdir(exist_ok=True)
    def warm(r):
        marker=unsupported/r.name
        if marker.is_file():return 0,1,0,[]
        try:v=slots(store.resolve(r))
        except CompileError as e:
            if e.code==3:
                atomic_write(marker,(str(e)+"\n").encode());return 0,1,0,[]
            return 0,0,0,[{"request":r.name,"base":True,"error":str(e)}]
        variants=[h for h,_ in patterns.get((r.wire,v),Counter()).most_common(8)]
        if r.wire==0 and v and all(not (item&~0x37f) for item in v):
            h=b"\1"*len(v)
            if h not in variants:variants.append(h)
        count=skipped=0;errors=[]
        for h in variants:
            req=Request.make(r.raw,r.wire,h);marker=unsupported/req.name
            if marker.is_file():skipped+=1;continue
            try:store.resolve(req);count+=1
            except CompileError as e:
                if e.code==3:atomic_write(marker,(str(e)+"\n").encode());skipped+=1
                else:errors.append({"request":req.name,"base":False,"error":str(e)})
        return count,0,skipped,errors
    seen=set();pending=set();typed=skipped=typed_skipped=0;errors=[]
    def collect(done):
        nonlocal typed,skipped,typed_skipped
        for f in done:
            n,s,t,e=f.result();typed+=n;skipped+=s;typed_skipped+=t;errors.extend(e)
    with ThreadPoolExecutor(jobs) as pool:
        for r in archive_requests(store,pak,None,str(compute_caps or 0),lambda st,k:st in ((0,1,2) if compute_caps is None else (5,))):
            if r.name in seen:continue
            seen.add(r.name);pending.add(pool.submit(warm,r))
            if len(pending)>=jobs*2:
                done,pending=wait(pending,return_when=FIRST_COMPLETED);collect(done)
        collect(pending)
    data=dict(originals=len(seen),typed=typed,unsupported_sources=skipped,unsupported_typed=typed_skipped,errors=errors,
              elapsed_s=round(time.monotonic()-start,3),**store.summary())
    if report:atomic_write(report,(json.dumps(data,indent=2)+"\n").encode())
    print("Cooked "+("graphics" if compute_caps is None else "compute")+":",{k:v for k,v in data.items() if k!="errors"},len(errors),"errors",flush=True)
    return data
