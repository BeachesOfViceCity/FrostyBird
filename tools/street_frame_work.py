#!/usr/bin/env python3
"""Describe the recorded GPU work leading into a verified shipping city frame."""
import argparse
import csv
import json
from pathlib import Path

from gpu_passes import classify
from street_timeline import StreetTimeline


def state(path):
    timeline=StreetTimeline()
    for line in path.read_text(errors="replace").splitlines():
        timeline.update(line)
    return timeline


def work(run,frame,types):
    section=state(run/f"street-window-{frame}.timeline")
    if not section.downtown:
        raise ValueError("frame has no movie-free city timeline")
    first=min(n for p in run.glob("street-window-*.timeline")
              if (n:=int(p.stem.rsplit("-",1)[1]))<=frame and state(p).downtown)
    # A work counter of N means N presents had already finished when this
    # command list was submitted. The new scanout is numbered N+1. Regular
    # work is sampled once per shader/command per 120-present epoch.
    last=frame-1
    start=max(first-1,last-119)
    with types.open() as file:
        names={}
        for r in csv.DictReader(file,delimiter="\t"):
            if r["name"]:names.setdefault(r["key"],set()).add(r["name"])
    passes={};exact=[];counts={}
    with (run/"gpu-work.tsv").open() as file:
        for r in csv.DictReader(file,delimiter="\t"):
            at=int(r["presented"])
            if not start<=at<=last:continue
            kinds=sorted(set().union(*(names.get(r[k],set()) for k in ("shader1","shader2","shader3"))))
            cmd=r["command"];counts[cmd]=counts.get(cmd,0)+1
            label={"build-tlas":"DXR / TLAS build","build-blas":"DXR / BLAS build",
                   "dispatch-rays":"DXR / DispatchRays"}.get(cmd) or classify(kinds)
            if not label and any(k.startswith("FNanite") for k in kinds):label="Nanite / material"
            if "FTonemapPS" in kinds:label="Tonemap"
            if not label:continue
            item={"presented":at,"submission":int(r["list_submission"]),"command":cmd,
                  "shaders":[r[k] for k in ("shader1","shader2","shader3") if int(r[k],16)],
                  "types":kinds,"arguments":[int(r[k]) for k in ("x","y","z")]}
            passes[label]=item
            if at==last:exact.append({"pass":label,**item})
    fenced={}
    if (run/"gpu-indirect.tsv").exists():
        with (run/"gpu-indirect.tsv").open() as file:
            for r in csv.DictReader(file,delimiter="\t"):
                at=int(r["presented"]);key=r["shader"];kinds=sorted(names.get(key,()))
                if not start<=at<=last or r["kind"]!="2":continue
                args=[int(v) for v in r["first_record"].split(",")]
                count=int(r["max_count"])
                if r["count_buffer"]!="-":count=min(count,int(r["count_buffer"]))
                if not count or len(args)!=3 or not all(args):continue
                label=classify(kinds)
                if label:fenced[label]={"presented":at,"shader":key,"types":kinds,
                                        "dispatch_groups":args,"effective_command_count":count}
    return {"frame":frame,"section":section.section,"first_verified_city_present":first,
            "sampled_work_counter_range":[start,last],"sample_counts_by_command":counts,
            "sampled_at_immediately_preceding_counter":exact,"latest_sample_by_pass":passes,
            "nonzero_fenced_indirect_by_pass":fenced}


if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("run",type=Path);cli.add_argument("frame",type=int);cli.add_argument("output",type=Path)
    cli.add_argument("--types",type=Path,default=Path(__file__).resolve().parents[1]/"build/gameplay-index/types.tsv")
    o=cli.parse_args();record=work(o.run,o.frame,o.types);o.output.write_text(json.dumps(record,indent=2)+"\n")
    print(f"{record['section']} present {o.frame}: {sum(record['sample_counts_by_command'].values())} sampled commands; "
          f"{len(record['sampled_at_immediately_preceding_counter'])} classified at counter {o.frame-1}")
