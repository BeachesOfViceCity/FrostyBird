#!/usr/bin/env python3
"""Compare a held title-window capture with the same fenced scanout and the supplied reference."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageOps
from street_timeline import StreetTimeline


def tonemap_matches(run,frame,scan):
    """The work tracer labels a pass with the preceding presentation counter."""
    h,w=scan.shape[:2];records=[]
    for p in run.glob(f"pass-61971c0a24686de1-{frame-1:06}-*.surf"):
        with p.open("rb") as f:
            fmt,pw,ph,pitch=struct.unpack("<4I",f.read(16))
            if fmt!=24 or (w,h)!=(pw,ph) or pitch<4*w or p.stat().st_size!=16+pitch*h:continue
            data=f.read()
        pixels=np.ndarray((h,w),dtype="<u4",buffer=data,strides=(pitch,4))
        match=np.ones((h,w),dtype=bool)
        for i in range(3):
            # Same integer conversion used by Presentation::blit for R10G10B10A2.
            match&=((pixels>>(10*i)&1023)*255//1023)==scan[:,:,i]
        records.append({"pass":p.name,"exact_rgb_pixels":int(match.sum()),"total_pixels":w*h})
    return records


def compare(run, frame, reference, output):
    stem=run/f"street-window-{frame}"
    winfile=stem.with_suffix(".png")
    gpufile=run/f"frame-{frame:06}.ppm"
    window=Image.open(winfile).convert("RGB")
    scan=Image.open(gpufile).convert("RGB")
    target=Image.open(reference).convert("RGB")
    meta=stem.with_suffix(".window").read_text()
    timeline=stem.with_suffix(".timeline").read_text()
    if not re.search(r'WM_CLASS.*frosty-xsx-shipping\.exe',meta,re.I):
        raise ValueError("window is not the shipping title")
    state=StreetTimeline()
    for line in timeline.splitlines():state.update(line)
    if state.section not in ("CityIntro","CityStreets"):
        raise ValueError(f"no downtown chase section in this frame's timeline: {state.section}")
    if state.open:
        raise ValueError("the title still has an open movie")
    # Presentation::blit reads the top-left texel in each pixel footprint.
    # At ordinary window sizes this gives a byte-exact independent witness.
    log=(run/"launch.log").read_text(errors="replace")
    window_binding=re.search(r"frosty/present: hwnd=(\w+) class=(UnrealWindowGDK) owner=(\d+) render=(\d+) surface=(\d+)x(\d+)",log)
    if not window_binding:raise ValueError("no presentation binding for the title's UnrealWindowGDK")
    pair=re.search(rf"holding window pair {frame} client=(\d+)x(\d+) dib=(\d+)x(\d+)",log)
    if not pair:raise ValueError("missing held-frame presentation dimensions")
    cw,ch,pw,ph=map(int,pair.groups())
    w,h=window.size;g=np.asarray(scan)
    if not (0<w<=cw and 0<h<=ch and pw and ph):raise ValueError("incompatible X11 and GDI dimensions")
    # Xwayland can clip the right/bottom of an oversized client at the
    # nested output boundary. The visible crop still has the client's pitch.
    yy=(np.arange(h)*ph//ch)*scan.height//ph
    xx=(np.arange(w)*pw//cw)*scan.width//pw
    predicted=g[yy[:,None],xx]
    delta=np.abs(np.asarray(window).astype(np.int16)-predicted)
    exact=int(np.all(delta==0,axis=2).sum())
    record={"frame":frame,"section":state.section,"window_size":window.size,"client_size":[cw,ch],"scanout_size":scan.size,
            "win32_class":window_binding[2],"hwnd":window_binding[1],"open_media_sessions":list(state.open),
            "window_metadata":meta.splitlines(),"rgb_mean_abs_error":float(delta.mean()),
            "exact_rgb_pixels":exact,"visible_pixels":w*h,"rgb_pixels_exact_fraction":exact/(w*h),
            "same_present_tonemap_samples":tonemap_matches(run,frame,g),
            "source_sha256":{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (winfile,gpufile,reference)}}
    output.mkdir(parents=True,exist_ok=True)
    (output/"measurement.json").write_text(json.dumps(record,indent=2)+"\n")
    if delta.mean()>3:
        raise ValueError(f"window/scanout mismatch: mean absolute RGB error {delta.mean():.2f}; see measurement.json")
    canvas=Image.new("RGB",(1600,500),(23,26,29));draw=ImageDraw.Draw(canvas)
    for x,img,caption in [(0,window,f"Retail game window | {state.section} | present {frame}"),(800,target,"User reference")]:
        scaled=ImageOps.contain(img,(780,445),Image.Resampling.LANCZOS)
        canvas.paste(scaled,(x+(800-scaled.width)//2,12+(445-scaled.height)//2))
        draw.text((x+12,468),caption,fill=(240,240,240))
    canvas.save(output/"comparison.png")
    print(json.dumps(record,indent=2))
    return record


if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("run",type=Path)
    cli.add_argument("frame",type=int)
    cli.add_argument("reference",type=Path)
    cli.add_argument("output",type=Path)
    a=cli.parse_args();compare(a.run,a.frame,a.reference,a.output)
