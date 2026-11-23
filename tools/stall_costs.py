#!/usr/bin/env python3
"""Intersect real compiler work with no-present gaps (including either end of a city window)."""
import argparse
import csv
import json
from pathlib import Path
import re

from frame_pacing import CITY_CHASE,intervals


def union(spans):
    out = []
    for a,b in sorted(spans):
        if b <= a:continue
        if out and a <= out[-1][1]:out[-1] = (out[-1][0],max(b,out[-1][1]))
        else:out.append((a,b))
    return out


def analyze(run, seconds):
    spans = intervals(run/"user/Temp/Logs/Frosty.log",CITY_CHASE)
    end = spans[0][0]+round(seconds*1e6)
    spans = [(a,min(b,end)) for a,b in spans if a<end]
    host=[];pso=[];counts={"translator":0,"dxc":0,"slow_demand_pso":0,"slow_replay_pso":0}
    def selected(a,b):
        return any(a<hi and b>lo for lo,hi in spans)
    with (run/"host-compiles.tsv").open() as f:
        for r in csv.DictReader(f,delimiter="\t"):
            a,b=int(r["begin_us"]),int(r["end_us"])
            if selected(a,b):
                counts["translator"]+=1;counts["dxc"]+=int(r["dxc"]);host.append((a,b))
    pattern=re.compile(r"frosty/pso-latency: .* total ([\d.]+) ms, driver [\d.]+ ms, (replay|demand) end_us=(\d+)")
    runtime=re.compile(r"frosty/runtime-latency: (allocation|submission) kind=(\d+) amount=(\d+) tid=(\d+) begin_us=(\d+) end_us=(\d+)")
    events=[]
    with (run/"launch.log").open(errors="replace") as f:
        for l in f:
            if m:=pattern.search(l):
                b=int(m[3]);a=b-round(float(m[1])*1000)
                if selected(a,b):
                    counts["slow_"+m[2]+"_pso"]+=1
                    if m[2]=="demand":pso.append((a,b))
            elif m:=runtime.search(l):
                a,b=int(m[5]),int(m[6])
                if selected(a,b):events.append(dict(op=m[1],kind=int(m[2]),amount=int(m[3]),tid=int(m[4]),begin_us=a,end_us=b))
    host,pso=union(host),union(pso)
    def overlap(lo,hi,ranges):
        return sum(max(0,min(hi,b)-max(lo,a)) for a,b in ranges)
    with (run/"frames.tsv").open() as f:
        reader=csv.DictReader(f,delimiter="\t")
        native_timed={"resource_alloc_wall_ms","queue_submit_wall_ms"}<=set(reader.fieldnames or ())
        rows=[(int(r["unix_us"]),int(r["present"])) for r in reader]
    stalls=[]
    for lo,hi in spans:
        before=lo
        for t,p in [(t,p) for t,p in rows if lo<=t<hi]+[(hi,None)]:
            if t-before>100_000:
                stalls.append({"end_present":p,"end_us":t,"ms":round((t-before)/1000,3),
                               "host_compile_ms":round(overlap(before,t,host)/1000,3),
                               "slow_demand_pso_ms":round(overlap(before,t,pso)/1000,3),
                               "either_ms":round(overlap(before,t,union(host+pso))/1000,3)})
            before=t
    result={"city":counts,"gaps_over_100ms":sorted(stalls,key=lambda x:-x["ms"])}
    if native_timed:
        for op in ("allocation","submission"):
            rs=union([(x["begin_us"],x["end_us"]) for x in events if x["op"]==op])
            for g in stalls:g["slow_"+op+"_ms"]=round(overlap(g["end_us"]-round(g["ms"]*1000),g["end_us"],rs)/1000,3)
        result["slow_runtime_calls"]=events
    return result


if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("run",type=Path)
    args=cli.parse_args();trial=json.loads((args.run/"result.json").read_text())
    if not trial["success"]:cli.error("expected a completed city trial")
    data=analyze(args.run,trial["target_seconds"]);(args.run/"stall-costs.json").write_text(json.dumps(data,indent=2)+"\n")
    print("City compiles:",data["city"]," | longest no-present gaps:",data["gaps_over_100ms"][:3])
