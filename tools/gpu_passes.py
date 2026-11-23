#!/usr/bin/env python3
"""Symbolize actual submitted D3D12X work against the package's shipping symbols."""
import argparse
from bisect import bisect_right
import csv
from pathlib import Path
from symbols import load


def classify(stack):
    for name in stack:
        if "HardwareRayTracing" not in name or "IndirectArgs" in name:continue
        for prefix,label in (("FLumenDirectLighting","direct lighting"),("FLumenRadianceCache","radiance cache"),
                             ("FLumenScreenProbeGather","screen-probe GI"),("FLumenReflection","reflections")):
            if name.startswith(prefix):return "Lumen HWRT / "+label
    for name in stack:
        if "FScreenProbeTraceMeshSDFsCS" in name:return "Lumen GI / mesh SDF trace"
        if "FReflectionTraceMeshSDFsCS" in name:return "Lumen reflections / mesh SDF trace"
    for name in stack:
        if "FHWRasterizePS" in name or "FMicropolyRasterizeCS" in name or "FVisiblityRasterComputeCS" in name:return "Nanite / cluster raster"
        if name.startswith("Nanite::") or any(s in name for s in ("FPersistentClusterCull_CS","FNodeAndClusterCull","FEmitSceneDepthStencilPS")):return "Nanite / geometry"
        if "MeshSDFObject" in name:return "Mesh SDF / object processing"
        if "Lumen" in name:return "Lumen / other"
    return ""


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("symbols",type=Path)
    parser.add_argument("trace",type=Path)
    parser.add_argument("--from-present",type=int,default=0)
    parser.add_argument("--all",action="store_true",help="include unclassified work")
    parser.add_argument("--types",type=Path,help="types.tsv from retail_shader_types.py")
    args=parser.parse_args()
    rows=load(args.symbols)
    types={}
    if args.types:
        with args.types.open() as f:
            for r in csv.DictReader(f,delimiter="\t"):
                if r["name"]:types.setdefault(r["key"],set()).add(r["name"])
    def symbol(addr):
        i=bisect_right(rows,addr,key=lambda r:r[0])-1
        if i>=0 and addr-rows[i][0]<rows[i][1]:return rows[i][2]
        return hex(addr)
    print("presented\tlist_submission\tpass\tcommand\tshader1\tshader2\twork_args\tpackage_type_names\tstack")
    with args.trace.open() as f:
        for r in csv.DictReader(f,delimiter="\t"):
            if int(r["presented"])<args.from_present:continue
            stack=[symbol(int(n,16)) for n in r["retail_recording_stack_rvas"].split(",") if n]
            names=sorted(set.union(set(),*(types.get(r[key],set()) for key in ("shader1","shader2","shader3"))))
            tag={"build-blas":"DXR / BLAS build","build-tlas":"DXR / TLAS build","dispatch-rays":"DXR / dispatch"}.get(r["command"]) or classify(names+stack)
            if not tag and not args.all:continue
            print("\t".join([r["presented"],r["list_submission"],tag or "other",r["command"],r["shader1"],r["shader2"],
                             ",".join(r[k] for k in ("x","y","z")),",".join(names)," <- ".join(stack)]))


if __name__=="__main__":main()
