#!/usr/bin/env python3
"""Prepare an offscreen depth-order test using the actual transition material PS."""
import argparse
from pathlib import Path
import struct
import subprocess
import zlib

from frosty import BUILD,compiler_tool

def without_forced_depth(raw):
    # Diagnostic control only: same machine code, signatures, resources and
    # render state. Clear just DEPTH_BEFORE_SHADER in its FSSM pixel descriptor.
    raw=bytearray(raw)
    def u(b,o):return struct.unpack_from("<I",b,o)[0]
    def q(b,o):return struct.unpack_from("<Q",b,o)[0]
    def put(b,o,v):struct.pack_into("<I",b,o,v)
    at=u(raw,32+4*(u(raw,28)-1))
    if raw[at:at+4]!=b"DXIL" or at+8+u(raw,at+4)!=len(raw):raise ValueError("unexpected DXIL extent")
    dx=raw[at+8:];rows=u(dx,32)
    if dx[24:28]!=b"XDAT" or [u(dx,rows+i*12) for i in range(u(dx,28))]!=[14,13,7]:raise ValueError("unexpected data table")
    off=u(dx,rows+16);n=u(dx,rows+20);tail=u(dx,rows+28)
    if dx[off:off+4]!=b"XBCH" or q(dx,off+8)+24!=n or tail!=((off+n+3)&~3):raise ValueError("unexpected machine object")
    obj=bytearray(zlib.decompress(dx[off+24:off+n]));control=16+q(obj,16+0x30)+0x18c
    if u(obj,control)!=0x1410:raise ValueError("expected the forced early, color-only UAV-writing material")
    put(obj,control,0x410);compressed=zlib.compress(obj)
    block=b"XBCH"+struct.pack("<IQQ",3,len(compressed),len(obj))+compressed;new_tail=(off+len(block)+3)&~3
    put(dx,rows+20,len(block));put(dx,rows+28,new_tail);put(dx,16,u(dx,16)+new_tail-tail)
    dx=dx[:off]+block+bytes(new_tail-off-len(block))+dx[tail:];put(dx,4,len(dx)//4)
    raw=raw[:at+8]+dx;put(raw,at+4,len(dx));put(raw,24,len(raw))
    return raw

def prepare(src,out,tool):
    out.mkdir(parents=True,exist_ok=True);control=out/"control.dxbc";control.write_bytes(without_forced_depth(src.read_bytes()))
    texts=[subprocess.check_output([tool,"source",p],text=True) for p in (control,src)]
    attribute="[earlydepthstencil]\n"
    if attribute in texts[0] or texts[1].count(attribute)!=1 or texts[1].replace(attribute,"")!=texts[0]:
        raise ValueError("control bit must affect only the pixel entry-point attribute")
    dxc=BUILD/"deps/dxc/bin/dxc"
    def compile(name,stage,source):
        p=out/(name+".hlsl");p.write_text(source)
        subprocess.run([dxc,"-E","main","-T",stage,"-O3","-Ges","-Gis","-Fo",out/(name+".dxil"),p],check=True)
    for name,s in zip(("late","early"),texts):
        entry="Output main(Input input) {"
        if s.count(entry)!=1 or "float4 r2 : SV_Position0;" not in s:raise ValueError("unexpected material interface")
        s="RWByteAddressBuffer witness : register(u1);\n"+s.replace(entry,entry+
          "\n    uint2 xy=uint2(input.r2.xy); uint sample_id=xy.y*256u+xy.x; witness.Store(sample_id*4u,0x2145df33u+sample_id);")
        compile(name,"ps_6_0",s)
    compile("triangle","vs_6_0","""
struct V { nointerpolation float4 uv:TEXCOORD3; nointerpolation uint lm:LIGHTMAP_ID0;
    nointerpolation uint eye:PACKED_EYE_INDEX0; nointerpolation uint view:PACKED_VIEW_INDEX0; float4 pos:SV_Position; };
V main(uint id:SV_VertexID){V v=(V)0;v.pos=float4(id==1?3:-1,id==2?3:-1,.5,1);return v;}
""")

if __name__=="__main__":
    c=argparse.ArgumentParser(description=__doc__);c.add_argument("shader",type=Path);c.add_argument("out",type=Path)
    c.add_argument("--compiler",type=Path,default=compiler_tool());a=c.parse_args();prepare(a.shader,a.out,a.compiler)
