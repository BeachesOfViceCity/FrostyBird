#!/usr/bin/env python3
"""Prepare a bounded read-only decode cache from the installed I/O Store.

The table selects original Zlib blocks. A previous storage trace or OS page
residency only predicts useful blocks; neither is trusted to supply data.
"""
import argparse
from collections import Counter
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import shutil
import struct
import tempfile
import zlib

HEADER=struct.Struct("<8sII32s")
ROW=struct.Struct("<QIIQ32s32s")
SAMPLE=struct.Struct("<QQIIII")

def publish(path,blocks,source):
    """Publish sorted (offset,input bytes,output bytes) extents atomically."""
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    index=[];start=HEADER.size+len(blocks)*ROW.size
    with tempfile.NamedTemporaryFile(dir=path.parent,prefix=".pack-",delete=False) as f:
        tmp=Path(f.name)
        try:
            f.seek(start)
            for off,compressed,decoded in blocks:
                payload=source(off,compressed)
                if len(payload)!=compressed:raise ValueError(f"short source at {off}")
                z=zlib.decompressobj();data=z.decompress(payload,decoded+1)
                if not z.eof or len(data)!=decoded:raise ValueError(f"invalid Zlib block at {off}")
                index.append(ROW.pack(off,compressed,decoded,f.tell(),hashlib.sha256(payload).digest(),hashlib.sha256(data).digest()))
                f.write(data)
            raw=b"".join(index);f.seek(0)
            f.write(HEADER.pack(b"XSDPACK1",len(index),ROW.size,hashlib.sha256(raw).digest()));f.write(raw)
            f.flush();os.fsync(f.fileno());os.replace(tmp,path)
        finally:tmp.unlink(missing_ok=True)

def resident(fd,size):
    page=mmap.PAGESIZE;n=(size+page-1)//page;bits=(ctypes.c_ubyte*n)()
    with mmap.mmap(fd,size,access=mmap.ACCESS_COPY) as m:
        addr=ctypes.addressof(ctypes.c_char.from_buffer(m));lib=ctypes.CDLL(None,use_errno=True)
        if lib.mincore(ctypes.c_void_p(addr),ctypes.c_size_t(size),bits):raise OSError(ctypes.get_errno(),"mincore")
    return bytes(bits)

def prepare(toc,out,limit,trace=None):
    from retail_mesh_fields import IoStore,u32
    from frame_pacing import CITY_CHASE,intervals
    s=IoStore(toc);hits=Counter();city=Counter()
    with s.data as src:
        size=os.fstat(src.fileno()).st_size
        if trace:
            p=Path(trace);spans=intervals(p/"user/Temp/Logs/Frosty.log",CITY_CHASE)
            report=json.loads((p/"result.json").read_bytes())
            if not report["success"] or not spans:raise ValueError("expected a completed moving-city trace")
            end=spans[0][0]+report["target_seconds"]*1_000_000
            data=(p/"storage-reads.bin").read_bytes()
            for t,off,c,d,_,_ in SAMPLE.iter_unpack(data[:len(data)//SAMPLE.size*SAMPLE.size]):
                hits[off,c,d]+=1
                if any(lo<=t<min(hi,end) for lo,hi in spans):city[off,c,d]+=1
            if not hits:raise ValueError("empty storage trace")
            by_offset={}
            for key in hits:by_offset.setdefault(key[0],[]).append(key)
        else:bits=resident(src.fileno(),size)
        candidates=[]
        for i in range(u32(s.toc,28)):
            row=s.toc[s.blocks+12*i:s.blocks+12*(i+1)]
            off=int.from_bytes(row[:5],"little");c=int.from_bytes(row[5:8],"little");d=int.from_bytes(row[8:11],"little")
            if row[11]>=len(s.methods) or s.methods[row[11]]!=b"Zlib" or not 0<c<=2<<20 or not 0<d<=2<<20 or off+c>size:continue
            key=(off,c,d)
            if trace:
                # Some reads include block padding from the same immutable
                # archive. Hash the complete observed input, padding included.
                eligible=[k for k in by_offset.get(off,()) if k[2]==d and c<=k[1]<=c+4095 and off+k[1]<=size]
                if not eligible:continue
                key=max(eligible,key=lambda k:(city[k],hits[k]))
                score=(bool(city[key]),(city[key]+hits[key]*.1)*c/d)
            else:
                if any(not b&1 for b in bits[off//mmap.PAGESIZE:(off+c-1)//mmap.PAGESIZE+1]):continue
                score=(False,c/d)
            candidates.append((score,key))
        chosen=[];total=HEADER.size
        for _,k in sorted(candidates,reverse=True):
            cost=ROW.size+k[2]
            if total+cost<=limit:chosen.append(k);total+=cost
        if not chosen:raise ValueError("no matching package blocks")
        out.parent.mkdir(parents=True,exist_ok=True)
        if shutil.disk_usage(out.parent).free<total+(1<<30):raise OSError("not enough space for the prepared block pack")
        chosen.sort();publish(out,chosen,lambda off,n:os.pread(src.fileno(),n,off))
    result=dict(blocks=len(chosen),candidates=len(candidates),pack_bytes=total,observed_city=sum(city.values()),
        predicted_city_hits=sum(city[k] for k in chosen),source=str(toc),selection="recorded" if trace else "resident")
    out.with_suffix(".json").write_text(json.dumps(result,indent=2)+"\n");return result

if __name__=="__main__":
    from frosty import BUILD,play_mount
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--game");parser.add_argument("--from-run",type=Path)
    parser.add_argument("--gib",type=float,default=2);parser.add_argument("--out",type=Path,default=BUILD/"play-runtime/storage/blocks.pack")
    a=parser.parse_args()
    if not 0<a.gib<=32:parser.error("--gib must be in (0,32]")
    print(json.dumps(prepare(play_mount(a.game)/"Frosty/Content/Paks/Frosty-XSX.utoc",a.out,int(a.gib*(1<<30)),a.from_run),indent=2))
