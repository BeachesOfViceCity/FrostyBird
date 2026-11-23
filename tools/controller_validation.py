#!/usr/bin/env python3
"""Exercise Xbox GameInput through a real Linux uinput gamepad and WineBus."""
import argparse
from contextlib import ExitStack
import fcntl
import os
from pathlib import Path
import queue
import subprocess
import threading
import time

ROOT=Path(__file__).resolve().parents[1]


class Peer:
    def __init__(self,cmd,env,log):
        self.p=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                                text=True,bufsize=1,env=env)
        self.lines=queue.Queue()
        def read():
            for line in self.p.stdout:
                line=line.rstrip();print(line,file=log,flush=True);self.lines.put(line)
            self.lines.put(None)
        self.reader=threading.Thread(target=read);self.reader.start()
    def wait(self,predicate,timeout=10):
        end=time.monotonic()+timeout
        while True:
            try:line=self.lines.get(timeout=max(0,end-time.monotonic()))
            except queue.Empty:raise RuntimeError("timed out waiting for "+str(self.p.args)) from None
            if line is None:raise RuntimeError(f"{self.p.args} exited: {self.p.wait()}")
            if predicate(line):return line
    def send(self,s):
        self.p.stdin.write(s+"\n");self.p.stdin.flush()
    def request(self,s):
        self.send(s);return self.wait(lambda l:l=="OK "+s)
    def close(self):
        if not self.p.stdin.closed:
            try:self.send("quit")
            except (BrokenPipeError,OSError):pass
            try:self.p.stdin.close()
            except BrokenPipeError:pass
        try:rc=self.p.wait(timeout=8)
        except subprocess.TimeoutExpired:self.p.kill();rc=self.p.wait()
        self.reader.join();self.p.stdout.close()
        return rc


def fixture_binary(out):
    binary=out/"uinput-test-pad"
    subprocess.run(["cc","-std=c17","-O2","-Wall","-Wextra",ROOT/"tools/uinput_test_pad.c","-o",binary],check=True)
    return binary


def check_fixture_id():
    # Feedback tests must address only the created fixture.
    for p in Path("/sys/class/input").glob("event*/device/id"):
        if (p/"vendor").read_text().strip().lower()=="054c" and (p/"product").read_text().strip().lower()=="0df2":
            raise RuntimeError("the fixture's 054c:0df2 hardware identity is already present")


def run(out,prefix):
    env={**os.environ,"WINEPREFIX":str(prefix.resolve()),"WINEDEBUG":"-all","WINEDLLOVERRIDES":"winedbg.exe=d"}
    env.pop("FROSTY_PAD_MAPPING",None)
    with ExitStack() as stack:
        # uinput devices are visible outside this test's Wine prefix.
        for p in sorted({prefix.resolve(),(ROOT/"build/codec-test-prefix").resolve()}):
            p.mkdir(parents=True,exist_ok=True);lock=stack.enter_context((p/".frosty-run.lock").open("a"))
            try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:raise SystemExit(f"Player active in {p}; no test controller created.") from None
        check_fixture_id();out.mkdir(parents=True,exist_ok=False);binary=fixture_binary(out)
        f=stack.enter_context((out/"guest.log").open("w"))
        guest=Peer([ROOT/"build/wine64/wine",ROOT/"build/guest/xs-controller-check.exe","054c","0df2","--exercise"],env,f)
        stack.callback(guest.close);guest.wait(lambda l:l=="READY",20)
        for plug,bus in enumerate(("usb","bluetooth"),1):
            log=stack.enter_context((out/(bus+".log")).open("w"))
            host=Peer([binary,bus],env,log)
            try:
                host.wait(lambda l:l=="READY");guest.request(f"attached {plug}")
                def frame(events):
                    host.send(" ".join(f"{kind} {code} {value}" for kind,code,value in events));host.wait(lambda l:l=="SENT")
                def state(mask,axes=(0,0,0,0,0,0)):
                    guest.request(f"state {mask:x} "+" ".join(str(v) for v in axes))
                state(0)
                # Author inputs in kernel/PlayStation layout, expectations in
                # GameInput layout. Nothing writes to a guest mapping or window.
                keys=[("Cross",0x130,4),("Circle",0x131,8),("Square",0x134,16),("Triangle",0x133,32),
                      ("Options",0x13b,1),("Create",0x13a,2),("L1",0x136,0x400),("R1",0x137,0x800),
                      ("L3",0x13d,0x1000),("R3",0x13e,0x2000)]
                for _,code,mask in keys:
                    frame([(1,code,1)]);state(mask);frame([(1,code,0)]);state(0)
                for x,y,mask in ((1,-1,0x240),(-1,1,0x180),(0,0,0)):
                    frame([(3,16,x),(3,17,y)]);state(mask)
                frame([(3,i,v) for i,v in enumerate((255,0,64,0,255,192))])
                state(0,(.25,.75,1,1,-1,-1))
                frame([(3,i,v) for i,v in enumerate((0,255,255,255,0,0))])
                state(0,(1,0,-1,-1,1,1))
                guest.request("history")
                guest.request("rumble 0.25 0.75")
                rumble=host.wait(lambda l:l.startswith("RUMBLE ") and l!="RUMBLE 0 0")
                strong,weak=map(int,rumble.split()[1:])
                if abs(strong-16384)>300 or abs(weak-49151)>300:raise ValueError("wrong rumble gains: "+rumble)
                host.wait(lambda l:l.startswith("PLAY ") and int(l.split()[2])>0)
                guest.request("rumble 0 0")
                # Some evdev drivers stop playback; SDL's Linux rumble
                # backend may instead replace the playing effect with zeros.
                end=host.wait(lambda l:l=="RUMBLE 0 0" or (l.startswith("PLAY ") and l.endswith(" 0")))
                if end=="RUMBLE 0 0":host.wait(lambda l:l.startswith("PLAY "))
            finally:rc=host.close()
            if rc:raise RuntimeError(f"uinput fixture exited {rc}")
            guest.request(f"removed {plug}")
            print(f"{bus}: 10 buttons, four D-pad directions, both sticks and independent triggers, "
                  f"rumble {strong}/{weak} + stop, history and hotplug passed",flush=True)
        if guest.close():raise RuntimeError("guest controller check failed")


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--out",type=Path,required=True)
    p.add_argument("--prefix",type=Path,default=ROOT/"build/play-validation/prefix")
    a=p.parse_args();run(a.out.resolve(),a.prefix)
