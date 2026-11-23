#!/usr/bin/env python3
"""Verify a command's stereo output on an isolated PipeWire/PulseAudio sink."""
import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import threading
import time
import uuid

import numpy as np


@contextmanager
def private_sink():
    name="xs_audio_"+uuid.uuid4().hex[:12]
    module=subprocess.check_output(["pactl","load-module","module-null-sink","sink_name="+name,"rate=48000",
                                    "channels=2","channel_map=front-left,front-right"],text=True).strip()
    try:yield name
    finally:subprocess.run(["pactl","unload-module",module],check=True)


def run(command,out,tones=False):
    rate=48000;block=2400
    out.mkdir(parents=True,exist_ok=False);stats={"frames":0,"audible_frames":0,"peak":0.,"left_tone_blocks":0,"right_tone_blocks":0}
    with private_sink() as sink:
        index=next(int(r["index"]) for r in json.loads(subprocess.check_output(["pactl","--format=json","list","short","sinks"]))
                   if r["name"]==sink)
        stop=threading.Event();errors=[]
        def sources():
            try:
                with (out/"sources.jsonl").open("w") as f:
                    previous=None
                    while not stop.is_set():
                        active=[]
                        for row in json.loads(subprocess.check_output(["pactl","--format=json","list","sink-inputs"])):
                            if int(row["sink"])!=index:continue
                            props=row["properties"];pid=props.get("application.process.id","")
                            try:exe=(Path("/proc")/str(int(pid))/"cmdline").read_bytes().split(b"\0")[0].decode(errors="replace")
                            except (OSError,ValueError):exe=""
                            active.append({"index":row["index"],"pid":pid,"command":exe,"app":props.get("application.name"),
                                           "binary":props.get("application.process.binary"),"format":row["sample_specification"],"corked":row["corked"]})
                        if active!=previous:
                            print(json.dumps({"unix_us":time.time_ns()//1000,"inputs":active}),file=f,flush=True);previous=active
                        stop.wait(.25)
            except Exception as e:errors.append(str(e))
        capture=subprocess.Popen(["parec","--raw","--device="+sink+".monitor","--channels=2","--rate=48000",
                                  "--format=float32le","--latency-msec=30","--client-name=XS-output-check"],stdout=subprocess.PIPE)
        def record():
            with (out/"levels.tsv").open("w") as f:
                f.write("unix_us\tframes\trms_left\trms_right\tpeak\n");carry=b""
                while data:=capture.stdout.read(block*8):
                    carry+=data
                    while len(carry)>=block*8:
                        x=np.frombuffer(carry[:block*8],dtype="<f4").reshape(block,2);carry=carry[block*8:]
                        rms=np.sqrt(np.mean(x.astype("float64")**2,axis=0));peak=float(np.max(np.abs(x)))
                        f.write(f"{time.time_ns()//1000}\t{block}\t{rms[0]:.8f}\t{rms[1]:.8f}\t{peak:.8f}\n")
                        stats["frames"]+=block
                        if not math.isfinite(peak) or not np.all(np.isfinite(rms)):stats["nonfinite"]=True;continue
                        stats["peak"]=max(stats["peak"],peak)
                        if max(rms)>0.0001:stats["audible_frames"]+=block
                        if tones:
                            spectrum=np.abs(np.fft.rfft(x,axis=0))
                            for ch,hz,key in [(0,1000,"left_tone_blocks"),(1,1500,"right_tone_blocks")]:
                                if rms[ch]>.009 and rms[ch]>12*rms[1-ch] and int(np.argmax(spectrum[:,ch]))==hz*block//rate:
                                    stats[key]+=1
        def read_checked():
            try:record()
            except Exception as e:errors.append(str(e))
        reader=threading.Thread(target=read_checked);reader.start()
        watcher=threading.Thread(target=sources);watcher.start()
        try:
            env=dict(os.environ,PULSE_SINK=sink)
            with (out/"command.log").open("w") as log:
                rc=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT).returncode
        finally:
            stop.set();watcher.join();capture.send_signal(signal.SIGTERM);reader.join()
            capture_status=capture.wait();capture.stdout.close()
            if capture_status not in (0,1,-signal.SIGTERM):errors.append(f"parec exit {capture_status}")
    stats.update(command_exit=rc,audible_seconds=round(stats["audible_frames"]/rate,3))
    if errors:stats["capture_errors"]=errors
    stats["passed"]=rc==0 and not errors and not stats.get("nonfinite") and stats["audible_seconds"]>0.25 and (
            not tones or min(stats["left_tone_blocks"],stats["right_tone_blocks"])>=7)
    (out/"result.json").write_text(json.dumps(stats,indent=2)+"\n");print(json.dumps(stats),flush=True)
    return 0 if stats["passed"] else 1


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--out",type=Path,required=True);p.add_argument("--tones",action="store_true")
    p.add_argument("command",nargs=argparse.REMAINDER);a=p.parse_args();cmd=a.command[1:] if a.command[:1]==["--"] else a.command
    if not cmd:p.error("expected -- COMMAND [ARGS...]")
    raise SystemExit(run(cmd,a.out,a.tones))
