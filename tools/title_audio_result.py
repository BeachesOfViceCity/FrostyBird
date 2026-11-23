#!/usr/bin/env python3
"""Attribute private-sink samples to the shipping process and its movie/gameplay timeline."""
import argparse
from bisect import bisect_right
import csv
import json
from pathlib import Path
import re
import subprocess
import numpy as np

from frame_pacing import CITY_CHASE,engine_us,intervals
from frosty import TITLE
from street_timeline import StreetTimeline


def report(capture,run,logo=None):
    engine=run/"user/Temp/Logs/Frosty.log";log=engine.read_text(errors="replace").splitlines()
    trial=json.loads((run/"result.json").read_text())
    spans=intervals(engine,CITY_CHASE)
    city_end=spans[0][0]+trial["target_seconds"]*1000000 if spans else 0
    sources=[json.loads(l) for l in (capture/"sources.jsonl").read_text().splitlines()]
    src_times=[e["unix_us"] for e in sources]
    known=set();pids=set();others=set();owned=[]
    for e in sources:
        active=False
        for i in e["inputs"]:
            key=(i["index"],i["pid"],i["app"],i["binary"])
            if i["command"].replace("\\","/").split("/")[-1].lower()==TITLE.lower():known.add(key)
            elif i["command"] or key not in known:
                others.add(i["command"] or f"unidentified input {i['index']} (pid {i['pid']})");continue
            # During shutdown PipeWire can retain the same input and client
            # PID after /proc/PID/cmdline disappears. Only an input already
            # positively identified in THIS capture inherits that identity.
            pids.add(i["pid"]);active|=not i["corked"]
        owned.append(active)
    states=[];times=[];state=StreetTimeline();logo_start=None
    for line in log:
        if "LogElectraPlayer:" not in line and "ChaseSection-" not in line:continue
        t=engine_us(line)
        if t is None:continue
        state.update(line)
        mode="movie" if any(opened[1] for opened in state.open.values()) else "city" if state.section in CITY_CHASE and not state.open else ""
        # Use the first movie's own playhead and wall timestamp for comparison.
        if not logo_start and (m:=re.search(r"Playback started at play position ([0-9.]+)",line)):logo_start=(t,float(m[1]))
        if not states or states[-1]!=mode:times.append(t);states.append(mode)
    groups={"movie":[],"city":[]};movie_t=[];movie_rms=[]
    for row in csv.DictReader((capture/"levels.tsv").read_text().splitlines(),delimiter="\t"):
        t=int(row["unix_us"]);j=bisect_right(src_times,t)-1;k=bisect_right(times,t)-1
        if j<0 or k<0 or not owned[j]:continue
        if states[k]=="city" and t>city_end:continue
        # Exclude transitions, decoder/endpoint queues and the shutdown tail.
        end=times[k+1] if k+1<len(times) else float("inf")
        if t-times[k]<300000 or end-t<300000 or states[k] not in groups:continue
        left,right=float(row["rms_left"]),float(row["rms_right"])
        groups[states[k]].append((int(row["frames"]),left,right,float(row["peak"])))
        if states[k]=="movie" and logo_start and t-logo_start[0]<30e6:
            movie_t.append((t-logo_start[0])/1e6+logo_start[1]);movie_rms.append((left+right)*.5)
    def stats(rows):
        return {"captured_s":round(sum(r[0] for r in rows)/48000,3),
                "audible_s":round(sum(r[0] for r in rows if max(r[1:3])>1e-4)/48000,3),
                "both_channels_s":round(sum(r[0] for r in rows if min(r[1:3])>1e-4)/48000,3),
                "peak":max((r[3] for r in rows),default=0)}
    out={name:stats(rows) for name,rows in groups.items()}
    if logo and len(movie_t)>50:
        raw=subprocess.check_output(["ffmpeg","-v","error","-i",logo,"-map","0:a:0","-t","30","-ac","2","-ar","48000","-f","f32le","-"])
        x=np.frombuffer(raw,dtype="<f4");block=2400;x=x[:len(x)//(2*block)*(2*block)].reshape(-1,block,2)
        ref=np.sqrt((x.astype("float64")**2).mean(axis=1)).mean(axis=1)
        t=np.asarray(movie_t);actual=np.asarray(movie_rms);best=None
        for dt in np.arange(-1.5,1.51,.025):
            idx=np.rint((t+dt)*20-.5).astype(int);keep=(idx>=0)&(idx<len(ref))
            if keep.sum()<50:continue
            a=actual[keep];b=ref[idx[keep]]
            if min(a.std(),b.std())<1e-5:continue
            corr=float(np.corrcoef(a,b)[0,1])
            if best is None or corr>best[0]:best=(corr,float(dt))
        if best:out["logo_envelope"]={"correlation":round(best[0],4),"alignment_s":round(best[1],3),"asset":logo.name}
    out.update(title_pids=sorted(pids),foreign_inputs=sorted(others))
    out["passed"]=bool(pids and not others and all(out[k]["both_channels_s"]>=1 for k in groups) and
                       json.loads((capture/"result.json").read_text())["passed"] and
                       trial["success"])
    return out


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("capture",type=Path);p.add_argument("run",type=Path)
    p.add_argument("--logo",type=Path);a=p.parse_args();result=report(a.capture,a.run,a.logo)
    text=json.dumps(result,indent=2);(a.capture/"title-result.json").write_text(text+"\n");print(text)
    raise SystemExit(0 if result["passed"] else 1)
