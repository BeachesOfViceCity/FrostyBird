#!/usr/bin/env python3
"""Stage checked raygens from the exact view contracts observed by the shipping bridge."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
from frosty import BUILD,compiler_tool
from shader_cache import ShaderCache,atomic_write

def stage(tool,cache,original,out,keys):
    if out.exists():raise ValueError(f"expected a new candidate directory: {out}")
    store=ShaderCache(tool,cache);contract=tuple(map(int,(original/"dispatch.contract").read_text().split()))
    products={};summary=[]
    for src in sorted(original.glob("*.xsrt")):
        if keys and src.stem not in keys:continue
        source=src.read_bytes();digest=hashlib.sha256(source).hexdigest()
        if not (original/(src.stem+".checked.plan")).is_file():continue
        counts=Counter();codes={}
        for f in store.recipes.glob(digest+".6.*"):
            req=store.recorded(f.name)
            if req.raw!=source or struct.unpack_from("<4I",req.params)!=contract:continue
            bound={int(reg):(int(dim),ty) for reg,dim,ty in (v.split(":") for v in req.params[16:].decode("ascii").split(","))}
            pkt=store.resolve(req);n,size=struct.unpack_from("<II",pkt);slots=struct.unpack_from("<"+"I"*n,pkt,8)
            accel=[r&0x17f for r in slots if r&0x20000]
            if len(accel)!=1 or bound[accel[0]][0]!=11:raise ValueError("missing exact live TLAS")
            views=tuple(sorted((r,*bound[r]) for r in slots if not r&0x20000))
            shape=(accel[0],views);code=pkt[8+4*n:]
            if size!=len(code) or codes.setdefault(shape,code)!=code:raise ValueError("inconsistent live binding projection")
            counts[shape]+=1
        if not counts:continue
        shape,_=counts.most_common(1)[0];accel,views=shape
        abi=(original/(src.stem+".rt.abi")).read_bytes()
        if struct.unpack("<II",abi)!=(4*contract[0],8):raise ValueError("different payload or attribute ABI")
        plan=" ".join(map(str,(*contract,accel,len(views))))+"\n"+"".join(f"{r} {d} {t}\n" for r,d,t in views)
        products[src.stem]=(plan.encode(),codes[shape])
        summary.append({"key":src.stem,"source_sha256":digest,"observed_requests":sum(counts.values()),
                        "distinct_used_layouts":len(counts),"selected_layout":views,"output_sha256":hashlib.sha256(codes[shape]).hexdigest()})
    if not products or set(keys)-products.keys():raise ValueError("no matching observed ray contract")
    # Publish private replacements by rename, never truncate a shared input.
    def link(src,dst):
        try:return os.link(src,dst)
        except OSError:return shutil.copy2(src,dst)
    shutil.copytree(original,out,copy_function=link)
    for key,(plan,dxil) in products.items():
        atomic_write(out/(key+".checked.plan"),plan);atomic_write(out/(key+".checked.dxil"),dxil)
    print(json.dumps(summary,indent=2));return summary

if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument("out",type=Path);cli.add_argument("--tool",type=Path,default=compiler_tool())
    cli.add_argument("--cache",type=Path,default=BUILD/"specializations");cli.add_argument("--source",type=Path,default=BUILD/"street-shadow-demand")
    cli.add_argument("--key",action="append",default=[]);cli.add_argument("--report",type=Path);a=cli.parse_args()
    data=stage(a.tool,a.cache,a.source,a.out,a.key)
    if a.report:atomic_write(a.report,(json.dumps(data,indent=2)+"\n").encode())
