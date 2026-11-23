#!/usr/bin/env python3
"""Build a source-matched raygen specialized to captured views. The GPU checks each trace's routing and payload."""
import argparse
import csv
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
def prepare(pak,run,source,out,checked):
    tool=ROOT/"build/host/frosty-shader";key=source.stem
    words,packed,word,value=checked
    with (ROOT/"build/hwrt-package/ray-sha-index.tsv").open() as f:
        records={r["collection_key"]:r for r in csv.DictReader(f,delimiter="\t")}
    r=records[key];args=[str(pak),r["library"],r["index"]]
    use=subprocess.check_output([tool,"rt-bindings",*args],text=True).splitlines()
    views={};capture=(run/"launch.log").read_text(errors="replace")
    for name,dim,fmt,stride in re.findall(r"frosty/dxr: space1 ([tu]\d+) table=\S+ dimension=(\d+) format=(\d+) stride=(\d+)",capture):
        v=tuple(map(int,(dim,fmt,stride)));prev=views.setdefault(name,v)
        if prev!=v:raise ValueError("mixed layouts: provide a capture of one dispatch")
    plan=[];layout=[];as_reg=None
    for line in use[1:]:
        space,reg,*rest=line.split();shape=" ".join(rest[:-1])
        if space!="space1":raise ValueError("unknown global register space")
        if shape=="CBV":continue
        dim,fmt,stride=views[reg]
        if shape=="TLAS":
            if dim!=11 or as_reg is not None:raise ValueError("unknown AS layout")
            as_reg=int(reg[1:]);continue
        expected={"buffer words":1,"Buffer":1,"RWTexture2D":4,"Texture2D":4,"Texture3D":8}[shape]
        if dim!=expected:raise ValueError(f"{reg}: wrong view dimension")
        if shape=="buffer words":ty=str(stride) if stride else "raw"
        elif fmt in (3,7,12,17,22,25,30,36,42,47,50,57,62):ty="uint"
        elif fmt in (4,8,14,18,32,38,43,52,59,64):ty="int"
        else:ty="float"
        layout.append(f"{reg}={ty}");plan.append(f"{int(reg[1:])+(256 if reg[0]=='u' else 0)} {dim} {ty}")
    if as_reg is None:raise ValueError("missing trace AS")
    out.mkdir(exist_ok=True,parents=True)
    subprocess.run([tool,"rt-lift-checked",*args,out,str(words),",".join(layout),*map(str,checked[1:])],check=True)
    if (out/(key+".xsrt")).read_bytes()!=source.read_bytes():raise ValueError("package record differs from captured collection")
    row=[words,packed,word,value,as_reg,len(plan)]
    tmp=out/(key+".rg.plan.tmp");tmp.write_text(" ".join(map(str,row))+"\n"+"\n".join(plan)+"\n")
    tmp.replace(out/(key+".checked.plan"))
    defaults=out/"dispatch.contract"
    if not defaults.exists():
        try:
            with defaults.open("x") as f:f.write(" ".join(map(str,checked))+"\n")
        except FileExistsError:pass
if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    for arg in ("pak","capture","source","out"):p.add_argument(arg,type=Path)
    p.add_argument("--checked",type=lambda s:int(s,0),nargs=4,required=True,metavar=("WORDS","ROUTE","FIELD","VALUE"),help="expected payload size in words, packed selector above mask, payload word index and value")
    a=p.parse_args();prepare(a.pak,a.capture,a.source,a.out,a.checked)
