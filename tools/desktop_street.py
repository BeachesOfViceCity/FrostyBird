#!/usr/bin/env python3
"""Measure the desktop-visible Gamescope client against a held shipping city present."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

import numpy as np
from PIL import Image

from street_timeline import StreetTimeline


def content(im):
    a=np.asarray(im.convert("RGB"))
    lit=np.max(a,axis=2)>18
    rows=np.flatnonzero(lit.mean(axis=1)>.03);cols=np.flatnonzero(lit.mean(axis=0)>.03)
    if not len(rows) or not len(cols):raise ValueError("no visible city pixels")
    return tuple(map(int,(cols[0],rows[0],cols[-1]+1,rows[-1]+1)))


def measure(run,frame,out):
    stem=run/f"street-window-{frame}";state=StreetTimeline()
    for line in stem.with_suffix(".timeline").read_text().splitlines():state.update(line)
    if not state.downtown or not (run/f"window-pair-{frame}.hold").exists():
        raise ValueError("city frame is not held by the original presenter")
    if f"paused on verified title present {frame};" not in (run/"launch.log").read_text(errors="replace"):
        raise ValueError("no pause confirmation from the shipping process")
    source=stem.with_suffix(".window").read_text()
    shipping=int(re.search(r"_NET_WM_PID\(CARDINAL\) = (\d+)",source)[1])
    if not Path(f"/proc/{shipping}").is_dir():raise ValueError("shipping process is no longer running")
    # Check the desktop compositor's own PID and launch arguments. A window
    # with a similar title (e.g. a screenshot viewer) is not the witness.
    ids=subprocess.check_output(["xdotool","search","--onlyvisible","--class","^gamescope$"],text=True).split()
    for xid in ids:
        props=subprocess.check_output(["xprop","-id",xid,"WM_CLASS","_NET_WM_NAME","_NET_WM_PID"],text=True)
        m=re.search(r"_NET_WM_PID\(CARDINAL\) = (\d+)",props)
        if not m:continue
        cmd=Path(f"/proc/{m[1]}/cmdline")
        if not cmd.exists():continue
        argv=os.fsdecode(cmd.read_bytes()).split("\0")
        if ("--backend" in argv and argv[argv.index("--backend")+1]=="sdl" and
            any(Path(a).name==run.name for a in argv if a)):
            break
    else:raise ValueError("no desktop Gamescope window for this run")
    subprocess.run(["xdotool","windowactivate","--sync",xid],check=True,timeout=5)
    subprocess.run(["xdotool","windowraise",xid],check=True,timeout=3)
    out.mkdir(exist_ok=True,parents=True);physical=out/"desktop-window.png"
    subprocess.run(["import","-window",xid,str(physical)],check=True,timeout=8)
    a=Image.open(stem.with_suffix(".png")).convert("RGB");b=Image.open(physical).convert("RGB")
    ba,bb=content(a),content(b)
    ca,cb=a.crop(ba),b.crop(bb);same=ca.size==cb.size;exact=None
    if same:exact=int(np.all(np.asarray(ca)==np.asarray(cb),axis=2).sum())
    def sample(im):return np.asarray(im.resize((320,132),Image.Resampling.BOX),dtype=float)
    u,v=sample(ca),sample(cb);mae=float(np.abs(u-v).mean());t=u-u.mean();s=v-v.mean()
    corr=float((t*s).sum()/np.sqrt((t*t).sum()*(s*s).sum()))
    result={"frame":frame,"section":state.section,"shipping_pid":shipping,"desktop_display":os.environ.get("DISPLAY"),
            "desktop_xid":xid,"desktop_properties":props.splitlines(),"desktop_window_size":b.size,
            "shipping_window_size":a.size,"shipping_content_bounds":ba,"desktop_content_bounds":bb,
            "equal_content_dimensions":same,"exact_content_pixels":exact,"thumbnail_rgb_mae_0_255":mae,
            "thumbnail_rgb_correlation":corr}
    (out/"desktop-measurement.json").write_text(json.dumps(result,indent=2)+"\n")
    if mae>6 or corr<.95:raise ValueError(f"desktop content differs from held title: RGB MAE {mae:.3f}, corr {corr:.4f}")
    print(json.dumps(result,indent=2))


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("run",type=Path);p.add_argument("frame",type=int);p.add_argument("output",type=Path)
    a=p.parse_args();measure(a.run,a.frame,a.output)
