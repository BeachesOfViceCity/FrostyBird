#!/usr/bin/env python3
"""One bounded retail CityIntro trial on the private Gamescope display. No image capture."""
import argparse
import ctypes
import fcntl
import json
import mmap
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import struct
import sys
import time

from frame_pacing import CITY_CHASE,engine_us,summarize
from frosty import BUILD,ROOT,play_mount,scheduling_arg,adapter_schedule,library

def expand_city_workers(prefix):
    """Private scheduling trial for the measured six-P/eight-E host.

    Leave the title's individually pinned game, render and RHI threads on
    their requested CPUs. Move only the observed broad worker pool to the
    other physical cores, including the six unused E cores.
    """
    from city_threads import private_title,read
    if os.sched_getaffinity(0)!=set(range(20)):return 0
    cpu=Path("/sys/devices/system/cpu")
    for i in range(20):
        siblings=read(cpu/f"cpu{i}/topology/thread_siblings_list")
        if siblings!=(f"{i&~1}-{i|1}" if i<12 else str(i)):return 0
    mask={i for i in range(20) if 0x3fd5&(1<<i)};count=0
    for task in (private_title(prefix)/"task").iterdir():
        try:
            tid=int(task.name)
            if os.sched_getaffinity(tid)==mask:os.sched_setaffinity(tid,set(range(6,20)));count+=1
        except ProcessLookupError:pass
    return count

def startup_sample(run,prefix):
    # Only on a stalled private boot. Sample the running game thread without
    # suspending Wine workers or requiring a debugger on the user's desktop.
    for proc in Path("/proc").glob("[0-9]*/cmdline"):
        try:
            if b"Frosty-XSX-Shipping.exe" not in proc.read_bytes().split(b"\0")[0] or b"WINEPREFIX="+os.fsencode(prefix) not in proc.with_name("environ").read_bytes().split(b"\0"):continue
            (run/"startup-maps.txt").write_text(proc.with_name("maps").read_text())
            a=bytearray(128);struct.pack_into("IIQQQ",a,0,1,128,1,10000000,1)
            struct.pack_into("Q",a,40,(1<<5)|(1<<6));buf=(ctypes.c_char*len(a)).from_buffer(a)
            fd=ctypes.CDLL(None).syscall(298,ctypes.byref(buf),int(proc.parent.name),-1,-1,8)
            if fd<0:return
            with mmap.mmap(fd,3*mmap.PAGESIZE,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE) as ring:
                os.close(fd);off,size=struct.unpack_from("QQ",ring,1040);tail=0;until=time.monotonic()+.8;pcs=[]
                while time.monotonic()<until:
                    head=struct.unpack_from("Q",ring,1024)[0]
                    while head-tail>=8:
                        j=tail%size;get=lambda n:ring[off+j:off+j+min(n,size-j)]+ring[off:off+max(0,n-(size-j))]
                        kind,_,n=struct.unpack("IHH",get(8))
                        if n<8 or n>head-tail:tail=head;break
                        if kind==9 and n==16:pcs.append(f"{struct.unpack_from('Q',get(n),8)[0]:x}")
                        tail+=n
                    struct.pack_into("Q",ring,1032,tail);time.sleep(.035)
                (run/"startup-pcs.txt").write_text("\n".join(pcs)+"\n")
            return
        except OSError:continue

def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("name");cli.add_argument("--seconds",type=int,default=90)
    cli.add_argument("--timeout",type=int,default=650);cli.add_argument("--pass-bridge",type=Path)
    cli.add_argument("--async-compute",action="store_true",help="use the shipping engine's all-eligible async compute policy")
    cli.add_argument("--stall-sample",action="store_true",help="capture one private thread dump after 15 seconds without a present")
    cli.add_argument("--cpu-samples",action="store_true",help="sample private title threads and Radeon counters during the city window")
    cli.add_argument("--frame-cpu",action="store_true",help="sample running game, render and RHI threads every 0.5 ms of userspace CPU time")
    cli.add_argument("--board",action="store_true",help="sample Radeon clocks and utilization during the city window")
    cli.add_argument("--stream-samples",action="store_true",help="sample the retail world's level-purge policy and process working set with board data")
    cli.add_argument("--storage-trace",action="store_true",help="record the coordinates of completed I/O Store blocks for offline preparation")
    cli.add_argument("--pc-workers",action="store_true",help="trial the spare physical cores for the measured shipping worker mask")
    cli.add_argument("--gpu-start",type=int,default=0,help="enable sampled GPU commands this many seconds after CityIntro")
    cli.add_argument("--gap-contexts",action="store_true",help="sample the pinned frame threads once in each recurrent city gap")
    cli.add_argument("--focus",help="restrict GPU timing to a comma-separated list of original shader keys")
    cli.add_argument("--schedule",action="append",type=scheduling_arg,default=[])
    cli.add_argument("--chase-purge-levels",type=int,choices=range(0,2049),metavar="N")
    cli.add_argument("--frame-pace",type=int,choices=(30,60),help="ask the shipping frame pacer for this rate")
    cli.add_argument("--game",default=os.environ.get("FROSTY_GAME_ROOT"));cli.add_argument("--inner",action="store_true",help=argparse.SUPPRESS)
    a=cli.parse_args()
    if not re.fullmatch(r"[\w.-]+",a.name) or a.name in (".","..","prefix") or a.seconds<=0:cli.error("invalid trial name or interval")
    if a.focus and (not a.pass_bridge or not re.fullmatch(r"[a-fA-F0-9]{16}(?:,[a-fA-F0-9]{16}){0,31}",a.focus)):cli.error("--focus needs a pass bridge and original shader keys")
    if a.gpu_start and (not a.pass_bridge or not 0<a.gpu_start<a.seconds):cli.error("--gpu-start needs a pass bridge and a time inside the window")
    root=ROOT;base=BUILD/"play-validation";base.mkdir(exist_ok=True)
    run=base/a.name;prefix=base/"prefix";user=run/"user"
    env=dict(os.environ);pak=play_mount(a.game);adapter_schedule(env)
    if not a.inner:
        with (base/".benchmark.lock").open("a") as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            run.mkdir()
            if not prefix.is_dir():subprocess.run(["cp","-a","--reflink=auto",BUILD/"codec-test-prefix",prefix],check=True)
            shutil.copytree(BUILD/"enter-verified-user",user,symlinks=True,ignore=shutil.ignore_patterns("Logs","Crashes","CrashReportClient"))
            (user/"Temp/Logs").mkdir(exist_ok=True,parents=True)
            env.pop("DISPLAY",None);env.pop("WAYLAND_DISPLAY",None)
            with (run/"compositor.log").open("w") as f:
                subprocess.run(["gamescope","--backend","headless","--prefer-vk-device","1002:73a5","-w","1280","-h","720","-W","1280","-H","720","--",
                                sys.executable,Path(__file__).resolve(),*sys.argv[1:],"--inner"],cwd=root,env=env,stdout=f,stderr=f)
        info=json.loads((run/"result.json").read_text());print(json.dumps(info,indent=2));return 0 if info["success"] else 1

    env.update(WINEDEBUG="-all,+debugstr,+loaddll",WINEDLLOVERRIDES="winedbg.exe=d;mscoree,mshtml=",VKD3D_DEBUG="warn",DXVK_LOG_LEVEL="info",
               DXVK_LOG_PATH=str(run),VKD3D_CONFIG=env.get("VKD3D_CONFIG","retain_psos"),FROSTY_PAD_MAPPING="Local\\FrostyPerf"+str(os.getpid()),
               FROSTY_HOST_COMPILE_TIMES=str(run/"host-compiles.tsv"))
    if a.pass_bridge:
        env["FROSTY_D3D12_XS"]=str(a.pass_bridge.resolve(strict=True))
        env["FROSTY_PASS_TIMES"]="Z:"+str(run/"pass-times.tsv").replace("/","\\")
        if a.focus:env["FROSTY_PASS_FOCUS"]=a.focus
    else:env.pop("FROSTY_PASS_TIMES",None);env.pop("FROSTY_D3D12_XS",None)
    private={**env,"WINEPREFIX":str(prefix),"WINEDEBUG":"-all"}
    times=run/"frames.tsv";engine=user/"Temp/Logs/Frosty.log";launch=run/"launch.log"
    cmd=[root/"Play","--game",pak,"--prefix",prefix,"--userdata",user,"--frame-times",times]
    if a.storage_trace:cmd+=["--storage-reads",run/"storage-reads.bin"]
    if a.async_compute:cmd.append("--async-compute")
    for setting in a.schedule:cmd+=["--schedule",setting]
    if a.chase_purge_levels is not None:cmd+=["--chase-purge-levels",str(a.chase_purge_levels)]
    if a.frame_pace:cmd+=["--",f'-ExecCmds="r.SetFramePace {a.frame_pace}"']
    if a.cpu_samples or a.frame_cpu:
        symbols=run/"symbols";symbols.mkdir()
        for dll in (a.pass_bridge or BUILD/"guest/d3d12_xs.dll",BUILD/"guest/xcrdapi.dll",BUILD/"guest/xmem.dll"):
            shutil.copy2(dll,symbols/dll.name)
        core=library(env.get("FROSTY_VKD3D",BUILD/"vkd3d"),"d3d12core.dll")
        shutil.copy2(core,symbols/core.name)
        for name in ("ntdll","kernelbase","winevulkan"):
            dll=BUILD/f"wine64/dlls/{name}/x86_64-windows/{name}.dll"
            if dll.is_file():shutil.copy2(dll,symbols/dll.name)
    start=time.monotonic();city=None;first=False;section=None;logo=False;last_pad=-100;movie=None;good=False;reason="timeout";reassigned=0;gpu_started=False
    pos={engine:0,launch:0};partial={engine:"",launch:""};win_pid=None;sampled=False;observer=None
    deadline_us=None;stream=None;stream_file=None;next_stream=0;frame_cache={}
    probes=((34,36),(36,40),(50,57),(70,72),(72,75),(76,78.7),(78.7,81),(86.5,89),(40,43));probed=set()
    lib=ctypes.CDLL(None);fd=lib.inotify_init1(os.O_CLOEXEC)
    for parent in (engine.parent,run):lib.inotify_add_watch(fd,os.fsencode(parent),2|8|128|256)
    def consume(p):
        if not p.is_file():return []
        with p.open("rb") as f:
            if f.seek(0,2)<pos[p]:pos[p]=0;partial[p]=""
            f.seek(pos[p]);s=partial[p]+f.read().decode(errors="replace");pos[p]=f.tell()
        lines=s.split("\n");partial[p]=lines.pop();return lines
    def press(button):
        subprocess.Popen([BUILD/"wine64/wine",BUILD/"guest/frosty-pad.exe",env["FROSTY_PAD_MAPPING"],str(button),"650"],
                         env=private,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def dump_stall():
        nonlocal sampled
        if not a.stall_sample or sampled or not win_pid:return
        sampled=True
        (run/"stalled-at.txt").write_text(str(time.time())+"\n")
        from city_threads import capture
        try:capture(prefix,run/"stall-profile",2)
        except (OSError,RuntimeError) as e:(run/"stall-sample-error.txt").write_text(str(e)+"\n")
        from shipping_waits import sample
        try:msg=f"{sample(prefix,win_pid,launch,run/'stalled-threads.tsv')} blocked title contexts"
        except (OSError,RuntimeError,ValueError) as e:msg=str(e)
        (run/"stalled-threads.log").write_text(msg+"\n")
    with launch.open("wb") as f:
        app=subprocess.Popen(cmd,cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT)
        try:
            while time.monotonic()-start<a.timeout:
                now=time.monotonic()
                for l in consume(launch):
                    if win_pid is None and (m:=re.search(r"^\d+\.\d+:([0-9a-f]+):[0-9a-f]+:\w+:vkd3d-proton:",l,re.I)):win_pid=int(m[1],16)
                    if "first GPU present to UnrealWindowGDK" in l:first=True
                for l in consume(engine):
                    if "Playback reached end" in l or ("IMediaPlayer::Close()" in l and not section):logo=True
                    if m:=re.search(r"Logging Custom Event ChaseSection-([\w-]+)",l):
                        section=m[1]
                        if a.pc_workers and section in ("WakeUp","CityIntro"):reassigned+=expand_city_workers(prefix)
                        with (run/"phases.tsv").open("a") as marks:marks.write(f"{time.time():.6f}\t{section}\t{l.strip()}\n")
                        if section=="CityIntro" and city is None:
                            city=now
                            stamp=engine_us(l)
                            if stamp is not None:deadline_us=stamp+a.seconds*1_000_000
                            if a.stream_samples:
                                # Only the private launcher's process is the
                                # title's ancestor under the normal ptrace policy.
                                from city_threads import private_title
                                from streaming_costs import StreamProbe
                                stream=StreamProbe(private_title(prefix),launch)
                                (run/"cpu-profile").mkdir(exist_ok=True)
                                stream_file=(run/"cpu-profile/streaming.tsv").open("w")
                                stream_file.write("unix_us\tpending_levels\tactive_threshold\tforce_on_streamout\tpartition_threshold\tgc_period_s\trss_bytes\tanon_bytes\tswap_bytes\n")
                                if stream.reason:(run/"stream-probe.log").write_text(stream.reason+"\n")
                            if a.cpu_samples or a.frame_cpu or a.board or a.stream_samples:
                                with (run/"cpu-samples.log").open("w") as cpu:
                                    observer=subprocess.Popen([sys.executable,root/"tools/city_threads.py",prefix,run/"cpu-profile",str(a.seconds),
                                                               *(["--frame-only"] if a.frame_cpu else [] if a.cpu_samples else ["--board-only"])],
                                                              stdout=cpu,stderr=subprocess.STDOUT)
                    if section=="WakeUp" and "Playback started at play position" in l and l.split(" Playback started")[0]!=movie:
                        press(16);movie=l.split(" Playback started")[0]
                if app.poll() is not None:reason=f"title exited {app.returncode}";break
                if a.gap_contexts and win_pid and section=="WakeUp" and not frame_cache:
                    from shipping_waits import sample
                    try:sample(prefix,win_pid,launch,run/"frame-probe-init.tsv",frame_threads=True,frame_cache=frame_cache)
                    except (OSError,RuntimeError,ValueError):frame_cache["unavailable"]=True
                if stream is not None and now>=next_stream:
                    try:stream_file.write("\t".join(map(str,[time.time_ns()//1000,*stream.sample()]))+"\n")
                    except (OSError,struct.error):pass
                    next_stream=now+.25
                if city is not None and a.pass_bridge and not gpu_started and now-city>=a.gpu_start:
                    (run/"pass-times.tsv.on").touch();gpu_started=True
                if city is not None and now-city>=a.seconds and first:
                    # A stopped title can have thousands of earlier presents.
                    # Require one real completion through the wall deadline;
                    # the confirming present is outside the measured window.
                    if times.is_file() and deadline_us is not None:
                        with times.open("rb") as stamps:
                            end=stamps.seek(0,2);stamps.seek(max(0,end-4096));lines=stamps.read().split(b"\n")
                        if any(len(cols:=line.split(b"\t"))>=3 and cols[1].isdigit() and int(cols[1])>=deadline_us for line in lines[1:-1]):
                            reason="city interval complete";good=True;break
                    if now-city>=a.seconds+5:reason="no successful present through city deadline";break
                if first and a.stall_sample and not sampled and times.is_file() and time.time()-times.stat().st_mtime>15:
                    dump_stall()
                if first and sampled and city is None and times.is_file() and time.time()-times.stat().st_mtime>55:
                    reason="no pre-city present for 55 seconds";break
                probe_gap=a.gap_contexts and city is not None and win_pid and next((i+1 for i,(lo,hi) in enumerate(probes) if lo<=now-city<hi and i+1 not in probed),0)
                if probe_gap and time.time()-times.stat().st_mtime>.07:
                    from shipping_waits import sample
                    probed.add(probe_gap);begin=time.time_ns()//1000
                    try:n=sample(prefix,win_pid,launch,run/f"gap-{probe_gap}.tsv",frame_threads=True,frame_cache=frame_cache)
                    except (OSError,RuntimeError,ValueError):n=0
                    with (run/"gap-context-times.tsv").open("a") as log:log.write(f"{probe_gap}\t{begin}\t{time.time_ns()//1000}\t{n}\n")
                if not first and now-start>50 and engine.exists() and time.time()-engine.stat().st_mtime>35:
                    startup_sample(run,prefix)
                    dump_stall();reason="startup stopped before first present";break
                if logo and not section and now-last_pad>=5:press(4);last_pad=now
                if select.select([fd],[],[],.012 if probe_gap else .5)[0]:os.read(fd,65536)
        finally:
            if stream is not None:stream.close()
            if stream_file is not None:stream_file.close()
            if app.poll() is None:subprocess.run([BUILD/"wine64/server/wineserver","-k"],env=private,timeout=15)
            try:app.wait(timeout=18)
            except subprocess.TimeoutExpired:app.terminate();app.wait(timeout=10)
            if observer is not None:
                try:observer.wait(timeout=4)
                except subprocess.TimeoutExpired:observer.terminate();observer.wait(timeout=4)
            os.close(fd)
    result={"success":good,"reason":reason,"first_present":first,"section":section,"sections":CITY_CHASE,"profiled":bool(a.pass_bridge),"async_override":a.async_compute,
             "radv_perftest":env.get("RADV_PERFTEST",""),"target_seconds":a.seconds,"wall_seconds":round(time.monotonic()-start,2)}
    result.update(cpu_sampled=a.cpu_samples or a.frame_cpu,frame_cpu_sampled=a.frame_cpu,board_sampled=a.cpu_samples or a.frame_cpu or a.board or a.stream_samples,stream_sampled=a.stream_samples,vkd3d_config=env["VKD3D_CONFIG"],
                   mesa_submit_thread=env.get("MESA_VK_ENABLE_SUBMIT_THREAD"),zero_chunks=env.get("FROSTY_ZERO_CHUNKS"),
                   vkd3d_override=env.get("FROSTY_VKD3D"),early_clear=env.get("FROSTY_EARLY_CLEAR"),shader_tool=env.get("FROSTY_SHADER_TOOL"),
                   upload_reserve=env.get("FROSTY_UPLOAD_RESERVE"),async_pool_reset=env.get("FROSTY_ASYNC_POOL_RESET"),keep_buffer_chunks=env.get("FROSTY_KEEP_BUFFER_CHUNKS"),chase_purge_levels=a.chase_purge_levels,
                   buffer_supply=env.get("FROSTY_BUFFER_SUPPLY"),
                  native_ordinal=env.get("FROSTY_NATIVE_GROUP_ORDINAL")=="1",compute_tool=env.get("FROSTY_COMPUTE_TOOL"),
                  ray_libraries=env.get("FROSTY_RT_DISPATCH_LIBRARIES"),sampled_stall=sampled,
                   gpu_focus=a.focus,gpu_start=a.gpu_start,worker_threads_reassigned=reassigned,requested_frame_pace=a.frame_pace,scheduling=a.schedule)
    if launch.is_file():
        result["origin_period_requests_us"]=[int(v) for v in re.findall(r"frosty/pacing: game requested (\d+) us",launch.read_text(errors="replace"))]
    if engine.is_file():
        keys=("r.RDG.AsyncCompute","r.Bloom.AsyncCompute","r.EnableAsyncComputeTranslucencyLightingVolumeClear",
               "r.ScreenPercentage","r.Nanite","r.Lumen.HardwareRayTracing","r.DeferUniformExpressionCaching","gc.MultithreadedDestructionEnabled",
               *(s.split("=",1)[0] for s in a.schedule))
        cfg=dict(re.findall(r"Setting CVar \[\[("+ "|".join(map(re.escape,keys))+r"):([^\]]+)\]\]",engine.read_text(errors="replace")))
        result["logged_settings"]=cfg;result["all_async"]=all(cfg.get(k)==v for k,v in zip(keys,("2","1","1")))
    if engine.is_file() and times.is_file():
        try:result["pacing"]=summarize(times,engine,CITY_CHASE,seconds=a.seconds if good else None)
        except (ValueError,KeyError) as e:
            result["timing_error"]=str(e)
            if good:result.update(success=False,reason=str(e))
    if good and "pacing" not in result:result.update(success=False,reason="no measured city presents")
    if good and (pacing:=result.get("pacing")) and (not pacing.get("present_observed_through_deadline") or pacing.get("requested_movie_free_s")!=a.seconds):
        result.update(success=False,reason="incomplete movie-free gameplay window")
    (run/"result.json").write_text(json.dumps(result,indent=2)+"\n");return 0 if result["success"] else 1

if __name__=="__main__":sys.exit(main())
