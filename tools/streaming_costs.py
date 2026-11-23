#!/usr/bin/env python3
"""Measure shipping level-purge scheduling, working-set peaks and presentation gaps."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import struct

from frame_pacing import CITY_CHASE,engine_us,intervals


class StreamProbe:
    """Read counters only from the private retail build with these instruction witnesses."""
    def __init__(self,proc,launch):
        self.proc=proc;self.handle=None;self.reason="no retail image base"
        text=launch.read_text(errors="replace")
        m=re.search(r'Loaded L"[^"\n]*\\Frosty-XSX-Shipping\.exe" at ([0-9a-f]+): native',text,re.I)
        if not m:return
        base=int(m[1],16)
        # RIP-relative loads in UWorld::UpdateLevelStreaming and
        # UWorldPartitionSubsystem::PostInitialize. Verify each reference in
        # the mapped image before interpreting these four 32-bit counters.
        refs=((0x515ce4e,"3905549baa02",2,"i"),(0x515ce2b,"8b0573dd7c02",2,"i"),
              (0x515ce8c,"833dfddc7c0200",2,"i"),(0x519ff53,"8b052fb37802",2,"i"),
              (0x50d0044,"c5fa103570b18502",4,"f"))
        try:
            with (proc/"mem").open("rb",buffering=0) as f:
                self.addresses=[]
                for off,code,disp,fmt in refs:
                    actual=os.pread(f.fileno(),len(code)//2,base+off)
                    if actual!=bytes.fromhex(code):
                        self.reason=f"different instruction at RVA {off:x}";return
                    self.addresses.append((base+off+len(actual)+struct.unpack_from("<i",actual,disp)[0],fmt))
                self.handle=os.dup(f.fileno());self.reason=""
        except OSError as e:self.reason=str(e)

    def sample(self):
        data=(self.proc/"status").read_text()
        mem=dict(re.findall(r"^(VmRSS|RssAnon|VmSwap):\s+(\d+) kB$",data,re.M))
        counters=[struct.unpack("<"+fmt,os.pread(self.handle,4,a))[0] for a,fmt in self.addresses] if self.handle is not None else [""]*5
        return [*counters,*[int(mem[k])*1024 if k in mem else "" for k in ("VmRSS","RssAnon","VmSwap")]]

    def close(self):
        if self.handle is not None:os.close(self.handle);self.handle=None


def analyze(run):
    result=json.loads((run/"result.json").read_bytes());engine=run/"user/Temp/Logs/Frosty.log"
    if not result["success"]:raise ValueError("expected a complete city trial")
    origin=intervals(engine,CITY_CHASE)[0][0];end=origin+result["target_seconds"]*1_000_000
    events={"gc_notifications":[],"streaming_teleports":[],"layer_unloads":[]}
    for line in engine.read_text(errors="replace").splitlines():
        key="gc_notifications" if "objects in NotifyUnreachableObjects" in line else "streaming_teleports" if "Detected Streaming Source Teleport" in line else "layer_unloads" if "Data Layer" in line and "-> Unloaded" in line else None
        if key and (us:=engine_us(line)) is not None and origin<=us<end:events[key].append(round((us-origin)/1e6,3))
    with (run/"frames.tsv").open() as f:rows=[r for r in csv.DictReader(f,delimiter="\t") if origin<=int(r["unix_us"])<end]
    out={**events,"ray_packets":{k:sum(int(r[k]) for r in rows) for k in ("ray_packet_reuses","ray_packet_allocations") if rows and k in rows[0]}}
    if rows and "upload_reserve_hits" in rows[0]:
        out["upload_reserve"]={k:sum(int(r[k]) for r in rows) for k in ("upload_reserve_hits","upload_reserve_misses")}
    for name,keys in (("board",("vram_bytes","client_gtt","evicted_vram")),("streaming",("pending_levels","active_threshold","force_on_streamout","partition_threshold","gc_period_s","rss_bytes","anon_bytes","swap_bytes"))):
        p=run/f"cpu-profile/{name}.tsv"
        if not p.is_file():continue
        with p.open() as f:samples=[r for r in csv.DictReader(f,delimiter="\t") if origin<=int(r["unix_us"])<end]
        out[name]={}
        for k in keys:
            parse=float if k=="gc_period_s" else int
            vals=[parse(r[k]) for r in samples if r.get(k)]
            if vals:out[name][k]={"min":min(vals),"max":max(vals),"last":vals[-1],"samples":len(vals)}
    return out

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("run",type=Path);a=parser.parse_args()
    result=analyze(a.run);(a.run/"streaming-costs.json").write_text(json.dumps(result,indent=2)+"\n")
    print("GC notifications:",len(result["gc_notifications"]),"teleports:",len(result["streaming_teleports"]),"layer unloads:",len(result["layer_unloads"]),result["ray_packets"],result.get("streaming",{}))
