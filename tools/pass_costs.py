#!/usr/bin/env python3
"""Rank timestamped XS commands from a FROSTY_PASS_PROFILE build, restricted to real movie-free city presents."""
import argparse
import collections
import csv
import json
import math
import statistics
from pathlib import Path

from frame_pacing import intervals,summarize

def report(run, types, records, window=None):
    result=run/"result.json";trial=json.loads(result.read_text()) if result.exists() else {}
    log=run/"user/Temp/Logs/Frosty.log";sections=trial.get("sections","CityIntro")
    pacing=summarize(run/"frames.tsv",log,sections,seconds=trial.get("target_seconds") if trial.get("success") else None)
    selected=None
    if window is not None:
        start,stop=window
        if not all(map(math.isfinite,window)) or not 0<=start<stop:raise ValueError("invalid recording-frame window")
        origin=intervals(log,sections)[0][0]
        with (run/"frames.tsv").open() as f:
            selected={int(r["present"]) for r in csv.DictReader(f,delimiter="\t")
                      if origin+round(start*1e6)<=int(r["unix_us"])<origin+round(stop*1e6)}
    names=collections.defaultdict(set)
    with types.open() as f:
        for row in csv.DictReader(f,delimiter="\t"):
            if row["name"]:names[row["key"]].add(row["name"])
    with records.open() as f:
        for line in f:
            lib,idx,stage,key,*_=line.rstrip().split("\t")
            if key not in names:names[key].add(f"{lib}[{idx}]")
    costs=collections.defaultdict(collections.Counter);counts=collections.Counter()
    spans=collections.defaultdict(list);rates=set()
    with (run/"pass-times.tsv").open() as f:
        for row in csv.DictReader(f,delimiter="\t"):
            if not row.get("gpu_hz"):continue
            frame=int(row["record_frame"])
            if selected is not None and frame not in selected:continue
            if not any(lo<=frame<hi for lo,hi in pacing["present_ranges"]):continue
            begin,end,hz=map(int,(row["begin_gpu"],row["end_gpu"],row["gpu_hz"]))
            if not begin or end<begin or not hz:continue
            ks=tuple(row[k] for k in ("shader1","shader2","shader3") if int(row[k],16))
            key=(row["queue_type"],row["command"],ks)
            costs[frame][key]+=(end-begin)*1000/hz;counts[key]+=1
            spans[frame].append((begin,end));rates.add(hz)
    if not costs:raise ValueError("no retired commands intersect the city interval")
    ranked=[]
    for (qt,op,ks),n in counts.items():
        ms=[v[qt,op,ks] for v in costs.values()]
        ranked.append({"queue":int(qt),"op":op,"keys":list(ks),"shader_names":["/".join(sorted(names.get(k,()))) or "unmapped" for k in ks],
                       "mean_ms":round(statistics.mean(ms),4),"median_ms":round(statistics.median(ms),4),"max_ms":round(max(ms),4),"calls":n})
    ranked.sort(key=lambda r:-r["mean_ms"])
    out={"pacing":pacing,"sampled_recording_frames":len(costs),"record_frame_ids":sorted(costs),"pass_costs":ranked}
    if window is not None:out["record_window_s"]=window
    # RADV timestamps on the direct and compute queues share one physical
    # time domain. Count concurrent ranges once. Recording-frame attribution
    # is sampled; these are not end-to-end presents or calibrated CPU times.
    if len(rates)==1:
        hz=rates.pop();busy=[]
        for ranges in spans.values():
            total=last=0
            for a,b in sorted(ranges):
                total+=max(0,b-max(a,last));last=max(last,b)
            busy.append(total*1000/hz)
        out["timed_gpu_union_ms"]={"median":round(statistics.median(busy),3),"mean":round(statistics.mean(busy),3)}
    return out

if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument("run",type=Path)
    cli.add_argument("--types",type=Path,default=Path("build/gameplay-index/types.tsv"))
    cli.add_argument("--records",type=Path,default=Path("build/street-city-shader-audit-107/records.tsv"))
    cli.add_argument("--window",nargs=2,type=float,metavar=("START","STOP"),help="only rank work recorded between these seconds from city entry")
    args=cli.parse_args();s=report(args.run,args.types,args.records,args.window)
    filename="pass-costs.json" if args.window is None else "pass-costs-%g-%g.json"%tuple(args.window)
    (args.run/filename).write_text(json.dumps(s,indent=2)+"\n")
    print("Movie-free timing:",json.dumps(s["pacing"]));print("GPU ms / sampled recording frame (%d frames; queues may overlap)"%s["sampled_recording_frames"])
    if "timed_gpu_union_ms" in s:print("Union of timed GPU commands (ms):",s["timed_gpu_union_ms"])
    for r in s["pass_costs"][:20]:
        print(f"{r['mean_ms']:8.3f}  q{r['queue']} {r['op']:16s} {' '.join(r['shader_names'])}  {' '.join(r['keys'])}")
