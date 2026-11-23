"""Sample saved Wine syscall frames of our own blocked validation title.

The private launcher is the Unix ancestor. No debugging attachment, thread
suspension or wineserver response is needed. Persist only code addresses,
never stack contents. Stack candidates are not a formal unwind.
"""
import bisect
import os
from pathlib import Path
import re
import struct
import subprocess
from city_threads import private_title,read

def sample(prefix,winpid,launch,out,limit=512,frame_threads=False,frame_cache=None):
    # File mappings and PE section headers are expensive to enumerate during
    # a short stall. For recurrent frame probes cache them in the private
    # runner before the city. Always validate the thread and current stack.
    cached=frame_cache is not None and "spans" in frame_cache
    p=frame_cache["proc"] if cached else private_title(prefix);maps=[];native=[];ntdll=None
    for line in ([] if cached else read(p/"maps").splitlines()):
        region,perms,offset,dev,inode,*file=line.split(maxsplit=5);a,b=(int(v,16) for v in region.split("-"));maps.append((a,b,perms))
        if file and file[0].startswith("/"):
            path=Path(file[0]);bias=a-int(offset,16)
            if "x" in perms and (".so" in path.name or path.name.startswith("libc-")):native.append((a,b,bias,path.name))
            if path.name=="ntdll.so" and int(offset,16)==0:ntdll=(bias,path)
    if cached:maps=frame_cache["maps"]
    unix_starts=[r[0] for r in native]
    def unix_label(ip):
        k=bisect.bisect_right(unix_starts,ip)-1
        if k>=0 and ip<native[k][1]:return f"{native[k][3]}+{ip-native[k][2]:x}"
        return ""
    starts=frame_cache["starts"] if cached else [m[0] for m in maps]
    def within(addr):
        k=bisect.bisect_right(starts,addr)-1
        return maps[k] if k>=0 and addr<maps[k][1] else (0,0,"")
    mods={} if cached else {int(a,16):path.split("\\")[-1] for path,a in re.findall(r'Loaded L"([^"\n]+)" at ([0-9a-f]+)',launch.read_text(errors="replace"),re.I)}
    with (p/"mem").open("rb",buffering=0) as mem,out.open("w") as f:
        def get(a,n):
            lo,hi,pr=within(a)
            if not pr.startswith("r") or n>hi-a or not 0<n<=16384:return b""
            try:return os.pread(mem.fileno(),n,a)
            except (OSError,OverflowError):return b""
        def u64(b,i):return struct.unpack_from("<Q",b,i)[0]
        # In a stalled Wine syscall the guest frame names the Windows call,
        # not the Unix lock owner. Resolve only this title's own ntdll locks
        # and report thread IDs and module-relative code candidates. The
        # pthread owner layout below is specific to the x86-64 glibc host.
        locks={};tasks=frame_cache["tasks"] if cached else list((p/"task").iterdir())
        if frame_threads and not cached:
            chosen=[]
            for t in tasks:
                try:
                    if t.name==p.name or os.sched_getaffinity(int(t.name)) in ({2},{4}):chosen.append(t)
                except ProcessLookupError:pass
            tasks=chosen
        if not frame_threads and ntdll and os.confstr("CS_GNU_LIBC_VERSION"):
            bias,path=ntdll
            symbols=subprocess.run(["nm","-an",path],capture_output=True,text=True,check=False).stdout
            owners={int(t.name) for t in tasks}
            for addr,name in re.findall(r"^([0-9a-f]+) [bBdD] (virtual_mutex|fd_cache_mutex)$",symbols,re.M):
                addr=bias+int(addr,16);data=get(addr,12)
                owner=struct.unpack_from("<i",data,8)[0] if len(data)==12 else 0
                locks[addr]=(name,owner if owner in owners else 0)
        if not frame_threads:
            with out.with_name("unix-waits.tsv").open("w") as unix:
                unix.write("unix_tid\twait_channel\tsyscall\twaiting_on\towns\tunix_stack_candidates\n")
                for t in tasks:
                    call=read(t/"syscall").split()
                    if len(call)<9:continue
                    syscall=int(call[0]);wchan=read(t/"wchan")
                    lock=locks.get(int(call[1],0),("",0))[0] if syscall==202 else ""
                    owns=",".join(n for n,owner in locks.values() if owner==int(t.name))
                    sp=int(call[-2],0);stack=get(sp,min(4096,within(sp)[1]-sp));candidates=[]
                    for off in range(0,len(stack)-7,8):
                        tag=unix_label(u64(stack,off))
                        if tag:candidates.append(f"{off}:{tag}")
                        if len(candidates)==32:break
                    unix.write(f"{t.name}\t{wchan}\t{syscall}\t{lock}\t{owns}\t{' '.join(candidates)}\n")
        spans=[]
        for base,name in mods.items():
            dos=get(base,64)
            if dos[:2]!=b"MZ":continue
            at=base+int.from_bytes(dos[60:64],"little");head=get(at,24)
            if head[:4]!=b"PE\0\0":continue
            count,=struct.unpack_from("<H",head,6);opt,=struct.unpack_from("<H",head,20)
            if not 0<count<=128:continue
            sections=get(at+24+opt,count*40)
            for i in range(0,len(sections),40):
                n,rva=struct.unpack_from("<II",sections,i+8);flags,=struct.unpack_from("<I",sections,i+36)
                if flags&0x20000000:spans.append((base+rva,base+rva+n,base,name))
        if cached:spans,code=frame_cache["spans"],frame_cache["code"]
        else:
            spans.sort();code=[r[0] for r in spans]
            if frame_threads and frame_cache is not None:
                frame_cache.update(proc=p,maps=maps,starts=starts,tasks=tasks,spans=spans,code=code)
        def label(ip):
            k=bisect.bisect_right(code,ip)-1
            return f"{spans[k][3]}+{ip-spans[k][2]:x}" if k>=0 and ip<spans[k][1] else ""
        f.write("unix_tid\twin_tid\twait_channel\tip\tstack_candidates\n");matched=0
        for task in tasks:
            call=read(task/"syscall").split()
            if len(call)<3:continue
            sp=int(call[-2],0)
            # This Wine build puts thread_data on a 64 KiB alignment just
            # before its signal stack and 1 MiB Unix syscall stack. Check
            # its TEB self pointer, process, TID and user stack bounds.
            for i in range(17):
                data=(sp&~65535)-i*65536;header=get(data,32)
                if len(header)!=32:continue
                teb=u64(header,0);tid,=struct.unpack_from("<I",header,28)
                if teb<=0xffff or not tid:continue
                tib=get(teb,0x380)
                if len(tib)!=0x380 or u64(tib,0x30)!=teb or u64(tib,0x40)!=winpid or u64(tib,0x48)!=tid:continue
                frame=get(u64(tib,0x378),0xb8)
                if len(frame)!=0xb8:continue
                rsp=u64(frame,0x88);top=u64(tib,8)
                if not u64(tib,16)<=rsp<top or rsp&7:continue
                stack=get(rsp,min(16384,top-rsp,within(rsp)[1]-rsp));candidates=[]
                for off in range(0,len(stack)-7,8):
                    tag=label(u64(stack,off))
                    if tag:candidates.append(f"{off}:{tag}")
                    if len(candidates)==64:break
                f.write(f"{task.name}\t{tid}\t{read(task/'wchan')}\t{label(u64(frame,0x70))}\t{' '.join(candidates)}\n")
                matched+=1;break
            if matched>=limit:break
    return matched
