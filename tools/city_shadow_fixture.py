#!/usr/bin/env python3
"""Pair the recorded downtown ray state with a source-matched checked raygen."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
from inspect_pipelines import ray_libraries
from shader_cache import atomic_write
from frosty import BUILD

def make(recipe,suite,plans,tool,out):
    raw=recipe.read_bytes();key=hashlib.sha256(raw).hexdigest()
    if recipe.stem!=key or struct.unpack_from("<II",raw)!=(0x31505358,3):raise ValueError("not a recorded ray pipeline")
    rg,libs=ray_libraries(raw);name,alias,_,off,reference=libs[rg];ident=alias.removeprefix("RG_")
    if len(libs)!=3 or ident==alias:raise ValueError("the city shadow probe needs a raygen, miss and triangle closest-hit")
    original=(plans/(ident+".xsrt")).read_bytes();plan=(plans/(ident+".checked.plan")).read_text().splitlines()
    words,route,word,value,accel,n=map(int,plan[0].split())
    views={int(reg):(int(dim),ty) for reg,dim,ty in (line.split() for line in plan[1:])}
    if (words,route,word,value,accel,n)!=(16,0x21,9,0x800000,6,7) or not views.keys()=={0,1,2,3,4,5,256}:raise ValueError("different checked city probe layout")
    views[accel]=(11,"float");layout=",".join(f"{r}:{d}:{t}" for r,(d,t) in sorted(views.items()))
    def compile(selector):
        data=subprocess.check_output([tool,"rt-rg-packet",str(words),str(selector),str(word),str(value),layout],input=original)
        count,size=struct.unpack_from("<II",data);code=data[8+4*count:]
        if len(code)!=size or code[:4]!=b"DXBC":raise ValueError("invalid checked raygen")
        return code
    candidate=compile(route)
    if candidate!=(plans/(ident+".checked.dxil")).read_bytes():raise ValueError("candidate code does not match the live checked layout")
    def link(src,dst):
        try:return os.link(src,dst)
        except OSError:return shutil.copy2(src,dst)
    shutil.copytree(suite,out,copy_function=link)
    rows=[]
    for nm,code in (("reference",reference),("candidate",candidate),("wrong",compile(route^1))):
        atomic_write(out/(nm+".dxil"),code);rows.append(f"{nm}.dxil {name}")
    for prefix,stage in (("MS_","miss"),("CH_","hit")):
        x,=[x for x in libs if x[1].startswith(prefix)];atomic_write(out/(stage+".dxil"),x[-1]);rows.append(stage+".dxil "+x[0])
    atomic_write(out/"city-shadow.libs",("\n".join(rows)+"\n").encode())
    b=raw[:off]+struct.pack("<I",len(candidate))+candidate+raw[off+4+len(reference):]
    native=out/"native";native.mkdir();atomic_write(native/(hashlib.sha256(b).hexdigest()+".plan"),b)

if __name__=="__main__":
    c=argparse.ArgumentParser(description=__doc__);c.add_argument("recipe",type=Path);c.add_argument("suite",type=Path)
    c.add_argument("plans",type=Path);c.add_argument("out",type=Path);c.add_argument("--tool",type=Path,default=BUILD/"host/frosty-shader")
    a=c.parse_args();make(a.recipe,a.suite,a.plans,a.tool,a.out)
