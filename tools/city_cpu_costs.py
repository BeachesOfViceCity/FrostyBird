#!/usr/bin/env python3
"""Symbolize private city CPU samples and intersect them with individual no-present gaps."""
import argparse
from bisect import bisect_right,bisect_left
from collections import Counter
import csv
import json
from pathlib import Path
import re
import struct
import subprocess

from frame_pacing import CITY_CHASE,intervals
from frosty import play_mount
from symbols import load

def analyze(run):
    profile=run/"cpu-profile";launch=(run/"launch.log").read_text(errors="replace")
    module={int(a,16):path.split("\\")[-1].lower() for path,a in re.findall(r'Loaded L"([^"\n]+)" at ([0-9a-f]+)',launch,re.I)}
    maps=[]
    for l in (profile/"maps.txt").read_text().splitlines():
        span,_,off,dev,inode,*name=l.split(maxsplit=5);a,b=[int(v,16) for v in span.split("-")]
        maps.append((a,b,Path(name[0]).name if name else "[anon]"))
    image="frosty-xsx-shipping.exe";game=load(play_mount(None)/"Symbols/Frosty-XSX-Shipping-Symbols.bin")
    funcs={};sizes={image:max(rva+n for rva,n,_ in game)}
    for f in (run/"symbols").glob("*.dll"):
        with f.open("rb") as h:
            h.seek(60);pe=struct.unpack("<I",h.read(4))[0]+24;h.seek(pe+24);bias=struct.unpack("<Q",h.read(8))[0]
            h.seek(pe+56);sizes[f.name.lower()]=struct.unpack("<I",h.read(4))[0]
        rows=[]
        for l in subprocess.check_output(["x86_64-w64-mingw32-nm","-n","-C",f],text=True,errors="replace",stderr=subprocess.DEVNULL).splitlines():
            if m:=re.match(r"^([0-9a-f]+) [tT] (.+)",l):rows.append((int(m[1],16)-bias,m[2]))
        funcs[f.name.lower()]=rows
    bases=sorted(module);starts=[v[0] for v in maps]
    title_ranges=[a for a,n in module.items() if n==image]
    def symbol(ip):
        # Controller helpers have separate address spaces but share the
        # launcher log. A helper image must not split the shipping PE range.
        for base in title_ranges:
            rva=ip-base
            if 0<=rva<sizes[image]:
                k=bisect_right(game,rva,key=lambda v:v[0])-1
                return image,game[k][2] if k>=0 and rva-game[k][0]<game[k][1] else ""
        j=bisect_right(bases,ip)-1;name="";fn=""
        if j>=0:
            name=module[bases[j]];rva=ip-bases[j]
            if rva<sizes.get(name,0):
                if name==image:
                    k=bisect_right(game,rva,key=lambda v:v[0])-1
                    if k>=0 and rva-game[k][0]<game[k][1]:fn=game[k][2]
                elif rows:=funcs.get(name):
                    k=bisect_right(rows,rva,key=lambda v:v[0])-1
                    if k>=0:fn=rows[k][1]
                return name,fn
        k=bisect_right(starts,ip)-1
        return (maps[k][2] if k>=0 and ip<maps[k][1] else "[unmapped]"),""
    trial=json.loads((run/"result.json").read_text());lo=intervals(run/"user/Temp/Logs/Frosty.log",CITY_CHASE)[0][0]
    seen={};samples=[]
    with (profile/"cpu.tsv").open() as f:
        for r in csv.DictReader(f,delimiter="\t"):
            t=int(r["unix_us"]);ip=int(r["ip"],16)
            if not lo<=t<lo+1e6*trial["target_seconds"]:continue
            if ip not in seen:seen[ip]=symbol(ip)
            samples.append((t,int(r["tid"]),*seen[ip]))
    samples.sort();times=[s[0] for s in samples]
    def rank(rows,limit):
        mods=Counter();functions=Counter();threads=Counter()
        for _,tid,module,fn in rows:
            mods[module]+=1;threads[tid]+=1
            if fn:functions[module+"!"+fn]+=1
        return {"samples":len(rows),"top_modules":mods.most_common(limit),"top_functions":functions.most_common(limit),"top_tids":threads.most_common(limit)}
    gaps=json.loads((run/"stall-costs.json").read_bytes())["gaps_over_100ms"]
    result={"whole":rank(samples,20),"sections":{},"gaps":[]};board=[]
    for name in CITY_CHASE:
        spans=[(max(lo,a),min(lo+1e6*trial["target_seconds"],b)) for a,b in intervals(run/"user/Temp/Logs/Frosty.log",name)]
        rows=[s for s in samples if any(a<=s[0]<b for a,b in spans)]
        if rows:result["sections"][name]=rank(rows,12)
    with (profile/"board.tsv").open() as f:
        for r in csv.DictReader(f,delimiter="\t"):board.append((int(r["unix_us"]),r))
    engine=[];io=[];ticks={}
    if (profile/"engines.tsv").is_file():
        with (profile/"engines.tsv").open() as f:engine=list(csv.DictReader(f,delimiter="\t"))
    if (profile/"active-threads.tsv").is_file():
        with (profile/"active-threads.tsv").open() as f:
            for r in csv.DictReader(f,delimiter="\t"):ticks.setdefault(int(r["tid"]),[]).append(tuple(map(int,(r["unix_us"],r["run_ns"],r["runnable_wait_ns"]))))
    if (profile/"process-io.tsv").is_file():
        with (profile/"process-io.tsv").open() as f:io=list(csv.DictReader(f,delimiter="\t"))
    for g in gaps[:24]:
        end=g["end_us"];begin=end-round(g["ms"]*1000);i=bisect_left(times,begin);j=bisect_left(times,end)
        row=dict(end_present=g["end_present"],end_seconds=round((end-lo)/1e6,3),ms=g["ms"],**rank(samples[i:j],7),
            board=[r for t,r in board if begin-50000<=t<=end+50000])
        if trial.get("frame_cpu_sampled"):
            row["by_thread"]={str(t):rank([s for s in samples[i:j] if s[1]==t],10) for t in {s[1] for s in samples[i:j]}}
        inside=[r for r in engine if begin<=int(r["begin_us"])<=int(r["end_us"])<=end]
        if len(inside)>=2 and len({r["clients"] for r in inside})==1 and int(inside[0]["clients"]):
            row["drm_accounted_ms"]={"observed_span":round((int(inside[-1]["end_us"])-int(inside[0]["begin_us"]))/1000,2),
                                     **{k:round((int(inside[-1][k+"_ns"])-int(inside[0][k+"_ns"]))/1e6,2) for k in ("gfx","compute","dma")}}
        ios=[r for r in io if begin<=int(r["unix_us"])<=end]
        if len(ios)>=2:row["process_io_delta"]={k:int(ios[-1][k])-int(ios[0][k]) for k in ("minor_faults","major_faults","read_bytes","write_bytes")}
        active=[]
        for tid,series in ticks.items():
            a=bisect_left(series,(begin,));b=bisect_left(series,(end+1,))
            if b-a<2:continue
            t0,run0,ready0=series[a];t1,run1,ready1=series[b-1]
            if t1-t0<.4*(end-begin):continue
            active.append(dict(tid=tid,span_ms=round((t1-t0)/1000,2),cpu_ms=round((run1-run0)/1e6,2),runnable_wait_ms=round((ready1-ready0)/1e6,2)))
        row["active_threads"]=sorted(active,key=lambda x:-x["cpu_ms"]);result["gaps"].append(row)
    if (run/"gap-context-times.tsv").is_file():
        result["frame_wait_probes"]=probes=[]
        def resolve(tag):
            m=re.fullmatch(r"(.+)\+([0-9a-f]+)",tag)
            if not m:return tag
            name=m[1].lower();rva=int(m[2],16);tab=game if name==image else funcs.get(name,[])
            k=bisect_right(tab,rva,key=lambda t:t[0])-1
            if k>=0 and (name!=image or rva-tab[k][0]<tab[k][1]):return name+"!"+tab[k][-1]
            return tag
        for l in (run/"gap-context-times.tsv").read_text().splitlines():
            i,begin,end,n=map(int,l.split());tasks=[]
            if n:
                with (run/f"gap-{i}.tsv").open() as f:
                    for t in csv.DictReader(f,delimiter="\t"):
                        candidates=[]
                        for pc in t["stack_candidates"].split():
                            _,_,tag=pc.partition(":");fn=resolve(tag)
                            if fn not in candidates:candidates.append(fn)
                        tasks.append(dict(tid=int(t["unix_tid"]),wait=t["wait_channel"],ip=resolve(t["ip"]),candidates=candidates[:28]))
            probes.append(dict(start_s=round((begin-lo)/1e6,3),sampling_ms=(end-begin)/1000,threads=tasks))
    return result

if __name__=="__main__":
    c=argparse.ArgumentParser(description=__doc__);c.add_argument("run",type=Path);a=c.parse_args()
    result=analyze(a.run);(a.run/"cpu-costs.json").write_text(json.dumps(result,indent=2)+"\n")
    for g in result["gaps"][:5]:print(g["end_seconds"],"s:",g["ms"],"ms,",g["samples"],"IPs:",[(x[:85],n) for x,n in g["top_functions"][:3]])
