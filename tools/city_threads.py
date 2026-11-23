#!/usr/bin/env python3
"""Bounded, read-only CPU and board sampler for a private moving-city profile."""
import argparse
import ctypes
import mmap
import os
from pathlib import Path
import re
import select
import struct
import time

def read(path):
    try:return path.read_text(errors="replace").strip()
    except OSError:return ""

class Ring:
    def __init__(self,tid,period=10_000_000):
        pg=mmap.PAGESIZE;a=bytearray(128)
        # Software task clock, userspace IP, TID and CLOCK_MONOTONIC stamp.
        struct.pack_into("IIQQQ",a,0,1,128,1,period,1|2|4)
        struct.pack_into("Q",a,40,(1<<5)|(1<<6)|(1<<25));struct.pack_into("i",a,92,time.CLOCK_MONOTONIC)
        buf=(ctypes.c_char*len(a)).from_buffer(a);lib=ctypes.CDLL(None,use_errno=True)
        fd=lib.syscall(298,ctypes.byref(buf),tid,-1,-1,8)
        if fd<0:raise OSError(ctypes.get_errno(),"perf_event_open")
        try:self.m=mmap.mmap(fd,3*pg,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE)
        finally:os.close(fd)
        self.off,self.size=struct.unpack_from("QQ",self.m,1040);self.tail=0;self.lost=0
    def drain(self):
        head=struct.unpack_from("Q",self.m,1024)[0];t=self.tail
        while head-t>=8:
            j=t%self.size
            def get(n):
                a=min(n,self.size-j)
                return self.m[self.off+j:self.off+j+a]+self.m[self.off:self.off+n-a]
            typ,_,n=struct.unpack("IHH",get(8))
            if n<8 or n>head-t or head-t>self.size:self.lost+=1;t=head;break
            if typ==9 and n>=32:
                ip,pid,tid,ns=struct.unpack_from("QIIQ",get(n),8);yield ns,tid,ip
            elif typ==2 and n>=24:self.lost+=struct.unpack_from("Q",get(n),16)[0]
            t+=n
        self.tail=t;struct.pack_into("Q",self.m,1032,t)

def private_title(prefix):
    for p in Path("/proc").glob("[0-9]*/cmdline"):
        try:
            if b"Frosty-XSX-Shipping.exe" in p.read_bytes().split(b"\0")[0] and b"WINEPREFIX="+os.fsencode(prefix) in p.with_name("environ").read_bytes().split(b"\0"):
                return p.parent
        except OSError:pass
    raise RuntimeError("private title process not found")

def capture(prefix,out,seconds,board_only=False,frame_only=False):
    target=private_title(prefix)
    out.mkdir(parents=True,exist_ok=True);board=next((p/"device" for p in Path("/sys/class/drm").glob("card[0-9]")
        if read(p/"device/vendor")=="0x1002" and read(p/"device/device")=="0x73a5"),None)
    hw=next((board/"hwmon").glob("hwmon*"),None) if board else None
    sensors=[board/p for p in ("gpu_busy_percent","mem_busy_percent","mem_info_vram_used")] if board else []
    if hw:sensors+=[hw/p for p in ("freq1_input","power1_average","temp2_input","freq2_input")]
    clients={}
    for fd in (target/"fd").iterdir():
        try:
            if not str(fd.readlink()).startswith("/dev/dri/"):continue
        except OSError:continue
        info=target/"fdinfo"/fd.name;text=read(info)
        if re.search(r"^drm-driver:\s*amdgpu$",text,re.M) and (m:=re.search(r"^drm-client-id:\s*(\d+)",text,re.M)):clients[m[1]]=info
    counters=("drm-engine-gfx","drm-engine-compute","drm-engine-dma","drm-memory-vram","drm-memory-gtt","amd-evicted-vram")
    def drm():
        fields=[0]*len(counters);n=0
        for k,path in clients.items():
            text=read(path);m=re.search(r"^drm-client-id:\s*(\d+)",text,re.M)
            if not m or m[1]!=k:continue
            n+=1
            for j,name in enumerate(counters):
                if x:=re.search(r"^"+name+r":\s*(\d+)\s*(ns|KiB|MiB|GiB|bytes?|B)?\s*$",text,re.M):
                    fields[j]+=int(x[1])*{"KiB":1<<10,"MiB":1<<20,"GiB":1<<30}.get(x[2],1)
        return [str(n),*[str(v) for v in fields]]
    rings={};denied=set();next_thread=next_board=0.;deadline=time.monotonic()+seconds;offset=time.time_ns()-time.monotonic_ns()
    last_run={};active=set()
    if not board_only:(out/"maps.txt").write_text(read(target/"maps")+"\n")
    lost=0;samples=0
    with os.fdopen(os.pidfd_open(int(target.name)),"rb",buffering=0) as handle, (out/"cpu.tsv").open("w") as cpu,(out/"threads.tsv").open("w") as sched,(out/"board.tsv").open("w") as gpu,(out/"engines.tsv").open("w") as engines,(out/"active-threads.tsv").open("w") as fast,(out/"process-io.tsv").open("w") as io:
        cpu.write("unix_us\ttid\tip\n");sched.write("unix_us\ttid\tname\trun_ns\trunnable_wait_ns\twchan\tlast_cpu\tallowed_mask\n")
        gpu.write("unix_us\tgpu_busy\tmem_busy\tvram_bytes\tgfx_hz\tpower_uw\tjunction_mc\tmem_hz\tclients\tgfx_ns\tcompute_ns\tdma_ns\tclient_vram\tclient_gtt\tevicted_vram\n")
        engines.write("begin_us\tend_us\tclients\tgfx_ns\tcompute_ns\tdma_ns\n");fast.write("unix_us\ttid\trun_ns\trunnable_wait_ns\n")
        io.write("unix_us\tminor_faults\tmajor_faults\tread_bytes\twrite_bytes\n")
        while (now:=time.monotonic())<deadline:
            if not board_only and now>=next_thread:
                stamp=time.time_ns()//1000;alive=set();deltas=[]
                try:tasks=list((target/"task").iterdir())
                except FileNotFoundError:break
                for t in tasks:
                    tid=int(t.name);v=read(t/"schedstat").split()
                    if len(v)<2:continue
                    stat=read(t/"stat").rpartition(") ")[2].split()
                    try:allowed=f"{sum(1<<c for c in os.sched_getaffinity(tid)):x}"
                    except ProcessLookupError:continue
                    alive.add(tid);sched.write(f"{stamp}\t{tid}\t{read(t/'comm')}\t{v[0]}\t{v[1]}\t{read(t/'wchan')}\t{stat[36] if len(stat)>36 else ''}\t{allowed}\n")
                    run=int(v[0]);deltas.append((run-last_run.get(tid,run),tid));last_run[tid]=run
                    selected=not frame_only or tid==int(target.name) or allowed in ("4","10")
                    if selected and tid not in rings and tid not in denied:
                        try:rings[tid]=Ring(tid,500_000 if frame_only else 10_000_000)
                        except OSError:denied.add(tid)
                for tid in rings.keys()-alive:
                    r=rings.pop(tid);lost+=r.lost;r.m.close()
                active={int(target.name),*(tid for _,tid in sorted(deltas,reverse=True)[:12])}
                next_thread=now+.75
            if not board_only:
                # fdinfo accounting advances on the driver's schedule. Keep
                # the actual bracketing times, not a made-up per-frame split.
                stamp=time.time_ns()//1000;v=drm()
                engines.write(f"{stamp}\t{time.time_ns()//1000}\t"+"\t".join(v[:4])+"\n")
                st=read(target/"stat").rpartition(") ")[2].split();devio=dict(re.findall(r"^(read_bytes|write_bytes): (\d+)$",read(target/"io"),re.M))
                if len(st)>9 and len(devio)==2:io.write(f"{time.time_ns()//1000}\t{st[7]}\t{st[9]}\t{devio['read_bytes']}\t{devio['write_bytes']}\n")
                for tid in active:
                    v=read(target/"task"/str(tid)/"schedstat").split()
                    if len(v)>=2:fast.write(f"{time.time_ns()//1000}\t{tid}\t{v[0]}\t{v[1]}\n")
            if now>=next_board:
                values=[read(s) for s in sensors];values+=[""]*(7-len(values))
                gpu.write(str(time.time_ns()//1000)+"\t"+"\t".join(values+drm())+"\n");next_board=now+.25
            for r in rings.values():
                for ns,tid,ip in r.drain():cpu.write(f"{(ns+offset)//1000}\t{tid}\t{ip:x}\n");samples+=1
            if select.select([handle],[],[],.25 if board_only else .024)[0]:break
    for r in rings.values():lost+=r.lost;r.m.close()
    print(f"{samples} user IP samples, {lost} lost, {len(denied)} unavailable threads",flush=True)

if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument("prefix",type=Path);cli.add_argument("out",type=Path);cli.add_argument("seconds",type=int)
    cli.add_argument("--board-only",action="store_true");cli.add_argument("--frame-only",action="store_true")
    args=cli.parse_args();capture(args.prefix.resolve(),args.out,args.seconds,args.board_only,args.frame_only)
