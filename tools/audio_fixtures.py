#!/usr/bin/env python3
"""Prepare standalone audio checks. Asset extraction is for validation only."""
import argparse
from pathlib import Path
import struct
import subprocess


def unpack_wave(wave):
    if wave[:4]!=b"RIFF" or wave[8:12]!=b"WAVE":raise ValueError("expected RIFF/WAVE")
    chunks={};pos=12;end=8+struct.unpack_from("<I",wave,4)[0]
    if end>len(wave):raise ValueError("truncated RIFF")
    while pos+8<=end:
        size=struct.unpack_from("<I",wave,pos+4)[0]
        if pos+8+size>end:raise ValueError("truncated chunk")
        chunks[wave[pos:pos+4]]=wave[pos+8:pos+8+size];pos+=8+size+(size&1)
    return chunks[b"fmt "],chunks[b"data"]


def package_wave(toc,name):
    from retail_mesh_fields import IoStore
    store=IoStore(toc);names=dict(store.files_in())
    def read(asset):
        i=names[asset];off=store.lengths+10*i
        n=int.from_bytes(store.toc[off+5:off+10],"big")
        if n>32*1024*1024:raise ValueError("select a small standalone effect")
        return bytes(store.read(i,0,n))
    header=read(name);data=read(str(Path(name).with_suffix(".ubulk")))
    for ch in (1,2):
        at=header.find(struct.pack("<HHI",0x166,ch,48000))
        fmt=header[at:at+52]
        if at>=0 and len(fmt)==52 and fmt[16:20]==b"\x22\0\x01\0":return fmt,data
    raise ValueError("no single-stream XMA2WAVEFORMATEX in selected header")


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--out",type=Path,required=True)
    group=p.add_mutually_exclusive_group();group.add_argument("--wave",type=Path,help="single-stream XMA2 RIFF file")
    group.add_argument("--iostore",type=Path,help="installed package .utoc for the optional effect fixture")
    p.add_argument("--asset",help="sound-wave .uasset in that container")
    a=p.parse_args()
    if bool(a.iostore)!=bool(a.asset):p.error("--iostore and --asset must be supplied together")
    a.out.mkdir(parents=True,exist_ok=True)
    ff=["ffmpeg","-y","-v","error"]
    for layout,hz in [("stereo",(440,880)),("5.1",(330,450,570,70,690,810))]:
        expr="|".join(f"0.06*sin(2*PI*{h}*t)" for h in hz)
        subprocess.run([*ff,"-f","lavfi","-i",f"aevalsrc={expr}:s=48000:c={layout}:d=1.2",
                        "-c:a","aac","-b:a","320k","-f","adts",a.out/(layout+".aac")],check=True)
    if a.wave or a.iostore:
        fmt,data=unpack_wave(a.wave.read_bytes()) if a.wave else package_wave(a.iostore,a.asset)
        if (len(fmt)!=52 or fmt[:2]!=b"\x66\x01" or fmt[16:20]!=b"\x22\0\x01\0" or
            not data or len(data)%2048):raise ValueError("expected whole packets in a single-stream XMA2 wave")
        body=b"WAVEfmt "+struct.pack("<I",len(fmt))+fmt+b"data"+struct.pack("<I",len(data))+data
        wave=a.out/"effect.xma";wave.write_bytes(b"RIFF"+struct.pack("<I",len(body))+body)
        (a.out/"effect.packets").write_bytes(data)
        subprocess.run([*ff,"-i",wave,"-f","s16le",a.out/"effect-reference.s16"],check=True)
        print(f"{len(data)//2048} XMA2 packets, {int.from_bytes(fmt[2:4],'little')} channels")


if __name__=="__main__":main()
