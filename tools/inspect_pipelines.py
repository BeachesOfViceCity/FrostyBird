#!/usr/bin/env python3
"""Compile selected recorded PSOs with the real RADV backend; count instructions without running the title."""
import argparse
import collections
import gzip
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from pipeline_families import split_graphics
from frosty import BUILD,ROOT

def fnv(raw):
    v=0xcbf29ce484222325
    for b in raw:v=(v*0x100000001b3&0xffffffffffffffff)^b
    return f"{v:016x}"

def ray_libraries(b):
    at=12+struct.unpack_from("<I",b,8)[0]+16
    def blob():
        nonlocal at
        n=struct.unpack_from("<I",b,at)[0];at+=4
        if n>len(b)-at:raise ValueError("truncated ray pipeline blob")
        s=b[at:at+n];at+=n;return s
    blob();rg,count=struct.unpack_from("<II",b,at);at+=8
    if not rg<count<=256:raise ValueError("invalid ray pipeline exports")
    libs=[]
    for i in range(count):
        entry,alias,group=(blob().decode("utf-16-le") for _ in range(3));offset=at
        libs.append((entry,alias,group,offset,blob()))
    if at!=len(b):raise ValueError("extra ray pipeline bytes")
    return rg,libs

if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument("out",type=Path);cli.add_argument("key",nargs="+")
    cli.add_argument("--recipes-dir",type=Path,default=BUILD/"specializations/pipeline-recipes-v1")
    args=cli.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    plans=args.out/"recipes";plans.mkdir();root=args.recipes_dir;wanted=set(args.key)
    for p in root.glob("*.plan"):
        b=p.read_bytes();magic,kind,n=struct.unpack_from("<3I",b)
        if magic!=0x31505358:continue
        if kind==1:keys={fnv(b[12+n+12:])}
        elif kind==3:
            rg,libs=ray_libraries(b);keys={libs[rg][1].removeprefix("RG_")}
        else:
            state=split_graphics(b)
            if not state:continue
            keys={fnv(s) for s in state[1:3] if s}
        match=wanted&keys
        if not match:continue
        wanted-=match;shutil.copy2(p,plans/p.name)
        for h in root.glob(p.stem+".*.hint"):shutil.copy2(h,plans/h.name)
        if not wanted:break
    if wanted:cli.error("missing recorded shader(s): "+", ".join(wanted))
    env={**os.environ,"WINEDEBUG":"-all","DXVK_LOG_LEVEL":"error","VKD3D_DEBUG":"err","FROSTY_TRACE_PSO":"1",
         "MESA_SHADER_CACHE_DISABLE":"true","VKD3D_SHADER_CACHE_PATH":"0","RADV_DEBUG":"shaders,shaderstats",
         "VK_DRIVER_FILES":"/usr/share/vulkan/icd.d/radeon_icd.x86_64.json"}
    summary=[];block=None;asm=False
    with gzip.open(args.out/"driver.log.gz","wt") as log,subprocess.Popen([sys.executable,ROOT/"tools/frosty.py",
            "pipeline-inspect","--suite",plans.resolve(),"--prefix",BUILD/"fork-prefix"],env=env,
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors="replace") as proc:
        for line in proc.stdout:
            log.write(line)
            if "frosty/pso-begin:" in line:
                    block={"label":line.strip(),"stages":[],"driver_reports":[]};summary.append(block);asm=False
            elif "frosty/pso-end:" in line:block=None;asm=False
            elif block is not None:
                if line.strip()=="disasm:":
                    asm=True;stage={"ops":collections.Counter(),"max_vgpr":-1,"max_sgpr":-1};block["stages"].append(stage);continue
                stat=re.fullmatch(r"(VGPRs|SGPRs|Spilled VGPRs|Spilled SGPRs|LDS size|Scratch size|Subgroups per SIMD|Code size):\s*(\d+)",line.strip())
                if stat:
                    if stat[1]=="SGPRs":block["driver_reports"].append({})
                    if block["driver_reports"]:block["driver_reports"][-1][stat[1]]=int(stat[2])
                if not asm:continue
                if line.isspace() and stage["ops"]:asm=False;continue
                m=re.match(r"^\s+([a-z][\w.]+)\s*(.*?)\s*;\s*[0-9a-f]{8}",line)
                if not m:continue
                op,operands=m.groups();stage["ops"][op]+=1
                for bank,name in (("v","max_vgpr"),("s","max_sgpr")):
                    regs=re.findall(r"\b"+bank+r"(\d+)\b|\b"+bank+r"\[\d+:(\d+)\]",operands)
                    for a,b in regs:stage[name]=max(stage[name],int(a or b))
        rc=proc.wait()
    for block in summary:
        if len(block["stages"])==len(block["driver_reports"])==1:block["stages"][0]["driver"]=block["driver_reports"][0]
        for st in block["stages"]:
            ops=st["ops"];st["instructions"]=sum(ops.values());st["scratch"]=sum(n for k,n in ops.items() if k.startswith("scratch_"))
            st["waits"]=sum(n for k,n in ops.items() if k.startswith("s_wait"));st["barriers"]=ops["s_barrier"]
            st["selects"]=sum(n for k,n in ops.items() if k.startswith("v_cndmask"))
            print(block["label"],{k:v for k,v in st.items() if k!="ops"})
    (args.out/"stats.json").write_text(json.dumps(summary,indent=2)+"\n")
    if rc or not any(s["stages"] for s in summary):raise SystemExit(rc or 1)
