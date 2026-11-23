#!/usr/bin/env python3
"""Convert an instrumented render target to a preview; the .surf file retains raw GPU bytes."""
import argparse
import struct
from pathlib import Path
import numpy as np
from PIL import Image

p=argparse.ArgumentParser(description=__doc__)
p.add_argument("input",type=Path);p.add_argument("output",type=Path)
p.add_argument("--exposure",type=float,default=1,help="linear exposure for float linear scene color")
args=p.parse_args();raw=args.input.read_bytes()
fmt,w,h,row=struct.unpack_from("<4I",raw);b=raw[16:]
if not w or not h or len(b)!=row*h:raise SystemExit("invalid pass capture")
if fmt in (27,28,29,87,88,90,91):
    a=np.frombuffer(b,dtype=np.uint8).reshape(h,w,4)[:,:,:3]
    if fmt in (87,88,90,91):a=a[:,:,::-1]
elif fmt in (23,24):
    x=np.frombuffer(b,dtype="<u4").reshape(h,w)
    a=np.stack([((x>>(10*k))&1023)/1023 for k in range(3)],axis=-1)*255.
elif fmt in (2,10,26):
    if fmt in (2,10):
        a=np.frombuffer(b,dtype="<f4" if fmt==2 else "<f2").reshape(h,w,4)[:,:,:3].astype(np.float32)
    else:
        x=np.frombuffer(b,dtype="<u4").reshape(h,w)
        def uf(v,bits):
            e=v>>bits;m=v&((1<<bits)-1)
            return np.where(e==0,np.ldexp(m.astype(float),1-15-bits),np.ldexp(1.+m/float(1<<bits),e.astype(int)-15))
        a=np.stack([uf(x&2047,6),uf((x>>11)&2047,6),uf(x>>22,5)],axis=-1)
    # Label this a diagnostic preview rather than the game's final tone mapping.
    a=np.clip(np.nan_to_num(a)*args.exposure,0.,1.)
    a=np.where(a<=.0031308,12.92*a,1.055*a**(1/2.4)-.055)*255.
else:raise SystemExit(f"unsupported RTV format {fmt}; raw data preserved")
Image.fromarray(np.rint(a).astype(np.uint8)).save(args.output)
print(f"RTV {w}x{h} DXGI {fmt}; output {args.output}")
