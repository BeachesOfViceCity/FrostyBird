#!/usr/bin/env python3
"""Developer-only direct route through the shipping feature showcase."""
import argparse
from contextlib import ExitStack
import csv
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from audio_monitor import run as monitor
from frame_pacing import engine_us
from frosty import BUILD,ROOT,TITLE,play_mount
from showcase_fixture import stage


def snapshot(run):
    ids=subprocess.run(["xdotool","search","--onlyvisible","--class","^frosty-xsx-shipping\\.exe$"],
                       capture_output=True,text=True,timeout=3)
    for xid in ids.stdout.split():
        props=subprocess.check_output(["xprop","-id",xid,"WM_CLASS","_NET_WM_PID"],text=True,timeout=3)
        pid=re.search(r"_NET_WM_PID\(CARDINAL\) = (\d+)",props)
        if not pid:continue
        exe=Path("/proc")/pid[1]/"cmdline"
        if exe.is_file() and os.fsdecode(exe.read_bytes().split(b"\0")[0]).lower().endswith("/"+TITLE.lower()):
            # X11's backing pixmap can be blank for a direct Vulkan present.
            # Ask this private Gamescope compositor for its actual scanout.
            subprocess.run(["gamescopectl","screenshot",str(run/"metasounds.png")],check=True,timeout=10)
            (run/"metasounds-window.txt").write_text(props);return


def inner(run,prefix,timeout,capture):
    user=run/"user";log=user/"Temp/Logs/Frosty.log";launch=run/"launch.log";frames=run/"presents.tsv"
    failures=run/"failed";failures.mkdir()
    env={**os.environ,"WINEDEBUG":"-all,+debugstr","WINEDLLOVERRIDES":"winedbg.exe=d;mscoree,mshtml=",
         "XS_DEBUG_FAILED_SHADERS":str(failures),
         "XS_DEBUG_FAILED_PSOS":"Z:"+str(failures).replace("/","\\"),
         "XS_DEBUG_PSO_KEYS":"Z:"+str(run/"accepted-graphics.tsv").replace("/","\\"),"DXVK_LOG_PATH":str(run)}
    private={**env,"WINEPREFIX":str(prefix),"WINEDEBUG":"-all"}
    env.pop("FROSTY_PAD_MAPPING",None)
    start=time.monotonic();outro=None;finished=None;reason="deadline"
    witness=[];events=[];last_pos=0;partial=b"";meta=None;restored=False;played=False;shot=False;ended=None
    director=json.loads((run/"shortcut.json").read_text())["director"];cues={};sequence_frame=-1
    cmd=[ROOT/"Play","--game",run/"package/Mount","--prefix",prefix,"--userdata",user,"--frame-times",frames]
    with launch.open("wb") as stream:
        app=subprocess.Popen(cmd,env=env,stdout=stream,stderr=subprocess.STDOUT)
        try:
            while time.monotonic()-start<timeout:
                if log.is_file():
                    with log.open("rb") as src:
                        src.seek(last_pos);lines=(partial+src.read()).split(b"\n");last_pos=src.tell();partial=lines.pop()
                    for raw in lines:
                        line=raw.decode(errors="replace").strip()
                        if "Logging Custom Event ChaseSection-" in line and "ChaseEnded" not in line:
                            reason="shortcut entered a chase section: "+line
                        if "PlayInternal - MovieSceneSequence: OutroMaster" in line:played=True
                        if (m:=re.search(r"Evaluating sequence OutroMaster at frame (\d+)",line)):sequence_frame=int(m[1])
                        if "Active device profile:" in line and line.endswith(" XSX_Outro"):
                            if outro:reason="unexpected second outro"
                            else:outro=engine_us(line)
                        if outro and (m:=re.search(r"Triggering event '([^']+)'",line)):
                            method=director.get(m[1],"")
                            if method.endswith("Callout"):
                                cues.setdefault(method,dict(unix_us=engine_us(line),frame=sequence_frame))
                            if method=="PlayMetaSoundsCallout":meta=engine_us(line)
                            if method=="SD_RestoreDefaultDeviceProfile" and meta:restored=True
                        if meta and ("Logging Custom Event SandboxIntroFinished" in line or
                                     ("Stop" in line and "MovieSceneSequence: OutroMaster" in line)):
                            ended=engine_us(line)
                        if meta and "Active device profile:" in line and line.endswith(" XSX"):restored=True
                        if ended and restored and finished is None:finished=engine_us(line)
                        if any(x in line for x in ("Logging Custom Event","Active device profile:", "r.AntiAliasingMethod =",
                                                   "CreatePipelineState(&","with error 800","Fatal error:","GPU Crashed")):
                            witness.append(line)
                        if outro and "Triggering event '" in line:events.append(line)
                        if "GPU Crashed or D3D Device Removed" in line or "Fatal error:" in line:reason="shipping fatal: "+line
                if reason!="deadline":break
                if app.poll() is not None:reason=f"title exited {app.returncode}";break
                if capture and meta and not shot and time.time_ns()//1000>=meta+3_250_000:
                    shot=True
                    try:snapshot(run)
                    except (OSError,subprocess.SubprocessError) as e:(run/"snapshot-error.txt").write_text(str(e))
                if finished and frames.exists():
                    with frames.open("rb") as f:
                        f.seek(max(0,f.seek(0,2)-4096));stamps=f.read().splitlines()
                    if any(len(k:=r.split(b"\t"))>2 and k[1].isdigit() and int(k[1])>=finished+5_000_000 for r in stamps[1:]):
                        reason="outro returned to gameplay";break
                time.sleep(.15)
        finally:
            if app.poll() is None:subprocess.run([BUILD/"wine64/server/wineserver","-k"],env=private,timeout=15)
            try:app.wait(timeout=15)
            except subprocess.TimeoutExpired:app.terminate();app.wait(timeout=10)
    (run/"showcase-events.log").write_text("\n".join(events)+"\n")
    (run/"timeline.log").write_text("\n".join(witness)+"\n")
    num=post_meta=post_end=0;last=0
    if frames.is_file():
        with frames.open() as f:
            for row in csv.DictReader(f,delimiter="\t"):
                if str(row.get("unix_us","")).isdigit():
                    last=int(row["unix_us"]);num+=bool(outro and last>=outro and (not finished or last<finished))
                    post_meta+=bool(meta and meta<=last and (not finished or last<finished));post_end+=bool(finished and last>=finished)
    expected={v for v in director.values() if v.endswith("Callout")}
    success=bool(reason=="outro returned to gameplay" and played and meta and outro<meta<=finished and
                 expected<=cues.keys() and num>0 and post_meta>0 and post_end>0 and not list(failures.iterdir()))
    result=dict(success=success,reason=reason,outro_us=outro,metasounds_us=meta,finished_us=finished,last_present_us=last,
                outro_presents=num,presents_from_metasounds=post_meta,presents_after_outro=post_end,
                sequence_events=len(events),callouts=cues,last_sequence_frame=sequence_frame,wall_s=round(time.monotonic()-start,2))
    (run/"result.json").write_text(json.dumps(result,indent=2)+"\n")
    return 0 if success else 1


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("name")
    p.add_argument("--game");p.add_argument("--timeout",type=int,default=320)
    p.add_argument("--screenshot",action="store_true",help="capture the private title window after the former MetaSounds failure")
    p.add_argument("--wait-for-player",action="store_true",help="wait for the normal player's session lock before starting the bounded replay")
    p.add_argument("--inner",action="store_true",help=argparse.SUPPRESS);a=p.parse_args()
    if not re.fullmatch(r"[\w-]+",a.name) or a.timeout<=0:p.error("invalid name or timeout")
    run=BUILD/"showcase-validation"/a.name;prefix=BUILD/"play-validation/prefix"
    if a.inner:return inner(run,prefix,a.timeout,a.screenshot)
    with ExitStack() as cleanup:
        prefix.parent.mkdir(parents=True,exist_ok=True)
        for path in (prefix.parent/".benchmark.lock",BUILD/"codec-test-prefix/.frosty-run.lock"):
            f=cleanup.enter_context(path.open("a"))
            try:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:
                if not a.wait_for_player:raise SystemExit(f"Session in use: {path.parent}; use --wait-for-player to queue the check.") from None
                print(f"Waiting for {path.parent} to close before starting the private replay.",flush=True)
                fcntl.flock(f,fcntl.LOCK_EX)
        if not prefix.is_dir():
            subprocess.run(["cp","-a","--reflink=auto",BUILD/"codec-test-prefix",prefix],check=True)
        run.mkdir(parents=True);cleanup.callback(shutil.rmtree,run/"package",ignore_errors=True)
        info=stage(play_mount(a.game),run)
        (run/"shortcut.json").write_text(json.dumps(info,indent=2)+"\n")
        cmd=["gamescope","--backend","headless","-w","1280","-h","720","-W","1280","-H","720","--",
             sys.executable,"-B",Path(__file__).resolve(),a.name,"--timeout",str(a.timeout),"--inner",*(["--screenshot"] if a.screenshot else [])]
        # Gamescope supplies DISPLAY for the child. The real player is neither
        # the input target nor the audio endpoint used for the measurement.
        for key in ("DISPLAY","WAYLAND_DISPLAY"):os.environ.pop(key,None)
        rc=monitor(cmd,run/"audio")
        if not (run/"result.json").exists():raise RuntimeError("validation did not launch; see audio/command.log")
        result=json.loads((run/"result.json").read_text())
        if result["finished_us"] and result["metasounds_us"]:
            window=result["callouts"].get("PlayMetaHumanCallout",{}).get("unix_us",result["finished_us"])
            with (run/"audio/levels.tsv").open() as f:
                levels=[r for r in csv.DictReader(f,delimiter="\t") if result["metasounds_us"]<=int(r["unix_us"])<window]
            result["metasounds_audio_seconds"]=round(sum(int(r["frames"]) for r in levels if
                max(float(r["rms_left"]),float(r["rms_right"]))>1e-4)/48000,3)
            result["success"] &= result["metasounds_audio_seconds"]>0
        result["success"] &= rc==0
        text=json.dumps(result,indent=2);(run/"result.json").write_text(text+"\n");print(text,flush=True)
        return int(not result["success"])


if __name__=="__main__":raise SystemExit(main())
