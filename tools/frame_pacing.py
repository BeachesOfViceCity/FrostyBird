#!/usr/bin/env python3
"""Report present spacing, excluding movie sessions and other game sections when a title log is supplied."""
import argparse
from bisect import bisect_left
import csv
from datetime import datetime,timezone
import json
import math
from pathlib import Path
import re
from statistics import median

from street_timeline import StreetTimeline

CITY_CHASE=("CityIntro","Frogger","CityStreets")
STAMP=re.compile(r"^\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d+)\]")

def engine_us(line):
    m=STAMP.match(line)
    if not m:return None
    return int(datetime.strptime(m[1],"%Y.%m.%d-%H.%M.%S:%f").replace(tzinfo=timezone.utc).timestamp()*1e6)

def intervals(log,section):
    selected={section} if isinstance(section,str) else set(section)
    state=StreetTimeline();begin=None;spans=[]
    with log.open(errors="replace") as f:
        for line in f:
            if "ChaseSection-" not in line and "IMediaPlayer::" not in line:continue
            t=engine_us(line)
            if t is None:continue
            state.update(line)
            active=state.section in selected and not state.open
            if active and begin is None:begin=t
            elif not active and begin is not None:spans.append((begin,t));begin=None
    if begin is not None:spans.append((begin,math.inf))
    return spans


def summarize(path,log=None,section="CityIntro",seconds=None):
    with path.open() as f:frames=list(csv.DictReader(f,delimiter="\t"))
    if len(frames)<2:raise ValueError("fewer than two successful presents")
    spans=intervals(log,section) if log else [(int(frames[0]["unix_us"]),math.inf)]
    if not spans:raise ValueError(f"no movie-free {section} in engine log")
    cutoff=spans[0][0]+seconds*1e6 if seconds is not None else math.inf
    samples=[];groups=[];window_us=0;window_presents=0;boundary_gaps=[];feedback=[];selected=[];slowest=None
    for lo,hi in spans:
        end=min(hi,cutoff);rows=[r for r in frames if lo<=int(r["unix_us"])<end]
        if seconds is not None and lo<end:
            window_us+=end-lo;window_presents+=len(rows)
            boundary_gaps.extend([(int(rows[0]["unix_us"])-lo)/1000,(end-int(rows[-1]["unix_us"]))/1000] if rows else [(end-lo)/1000])
            feedback.extend(int(r["last_display_stat"]) for r in rows if r.get("last_display_stat"))
            stamps=[int(r["unix_us"]) for r in rows];selected.extend(stamps)
            # For complete rolling seconds inside a movie-free span the count
            # can only fall when a present leaves the left side. Check that
            # instant at the timestamp resolution, including either boundary.
            if end-lo>=1_000_000:
                for start in [lo,*(t+1 for t in stamps if t+1<=end-1_000_000),int(end-1_000_000)]:
                    item=(bisect_left(stamps,start+1_000_000)-bisect_left(stamps,start),start)
                    if slowest is None or item<slowest:slowest=item
        if len(rows)<2:continue
        groups.append([int(rows[0]["present"]),int(rows[-1]["present"])])
        # The first completion in each span may include time in another shot.
        samples.extend(rows[1:])
    if not samples:raise ValueError("fewer than two presents in the selected segment")
    dt=sorted(float(r["interval_ms"]) for r in samples)
    worst=max(samples,key=lambda r:float(r["interval_ms"]))
    result={"section":section if log else None,"present_ranges":groups,"completed_intervals":len(dt),
            "covered_seconds":round(sum(dt)/1000,3),"mean_fps_over_completed_intervals":round(1000*len(dt)/sum(dt),3),
            "median_ms":round(median(dt),3),"p95_ms":dt[math.ceil(.95*len(dt))-1],"p99_ms":dt[math.ceil(.99*len(dt))-1],"worst_ms":dt[-1],
            "over_100_ms":sum(x>100 for x in dt),"over_250_ms":sum(x>250 for x in dt),"over_1s":sum(x>=1000 for x in dt),
            "at_or_below_33_33_ms":sum(x<=1000/30 for x in dt),
            "worst_interval":{k:v for k,v in worst.items() if k=="present" or k.endswith("_ms")},
            "last_display_stat":int(samples[-1].get("last_display_stat",0))}
    if window_us:
        result.update(requested_window_s=seconds,requested_movie_free_s=round(window_us/1e6,3),window_presents=window_presents,
                      fps_over_requested_window=round(window_presents/seconds,3),
                      fps_over_movie_free_time=round(window_presents*1e6/window_us,3),
                      boundary_gaps_ms=[round(v,3) for v in boundary_gaps],
                      longest_without_recorded_present_ms=round(max(dt[-1],*boundary_gaps),3),
                      window_gaps_over_100_ms=result["over_100_ms"]+sum(x>100 for x in boundary_gaps),
                      window_gaps_over_250_ms=result["over_250_ms"]+sum(x>250 for x in boundary_gaps),
                      window_gaps_at_least_1s=result["over_1s"]+sum(x>=1000 for x in boundary_gaps))
        if feedback:result["dxgi_feedback_advance"]=feedback[-1]-feedback[0]
        result["present_observed_through_deadline"]=int(frames[-1]["unix_us"])>=cutoff
        if slowest:result["slowest_movie_free_second"]={"presents":slowest[0],"start_s":round((slowest[1]-spans[0][0])/1e6,6)}
        result["wall_time_bins"]=bins=[]
        for off in range(0,round(seconds*1e6),15_000_000):
            t=spans[0][0]+off;end=min(cutoff,t+15_000_000)
            n=bisect_left(selected,end)-bisect_left(selected,t)
            bins.append(dict(start_s=off/1e6,seconds=(end-t)/1e6,presents=n,fps=round(n*1e6/(end-t),3)))
    return result


if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("times",type=Path);cli.add_argument("--engine-log",type=Path)
    cli.add_argument("--section",default="CityIntro",help="one section or comma-separated adjoining gameplay sections");cli.add_argument("--seconds",type=float)
    a=cli.parse_args();sections=tuple(a.section.split(",")) if "," in a.section else a.section
    print(json.dumps(summarize(a.times,a.engine_log,sections,a.seconds),indent=2))
