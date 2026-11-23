#!/usr/bin/env python3
"""Join Frosty's frozen global shader map, code archive, and shipping symbols."""
import argparse
import csv
import ctypes
from ctypes.util import find_library
from pathlib import Path
import re
import struct
import subprocess
from symbols import load


def shader_names(symbols):
    # FHashedName uppercases the ASCII comparison name and calls
    # CityHash64WithSeed(bytes, length, FName.Number). Class names have Number=0.
    lib=find_library("absl_city")
    if not lib:raise ValueError("install the Abseil CityHash library to resolve type names")
    dll=ctypes.CDLL(lib)
    # The Abseil C++ symbol contains a distribution-dependent LTS namespace.
    libs=subprocess.check_output(["ldconfig","-p"],text=True)
    path=next(line.split(" => ")[-1] for line in libs.splitlines() if line.strip().startswith(lib+" "))
    table=subprocess.check_output(["nm","-D","--defined-only",path],text=True)
    fn=getattr(dll,next(line.split()[-1] for line in table.splitlines() if "CityHash64WithSeedE" in line))
    fn.restype=ctypes.c_uint64;fn.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.c_uint64]
    names=set()
    for _,_,s in load(symbols):
        for suffix in ("::ConstructSerializedInstance","::ConstructCompiledInstance","::ShouldCompilePermutationImpl"):
            if s.endswith(suffix):names.add(s.removesuffix(suffix))
        if (m:=re.fullmatch(r"`dynamic initializer for '(.+)::StaticType''",s)):names.add(m[1])
    # The serialized shader type can omit the implementation's C++ namespace.
    names.update(n.rsplit("::",1)[-1] for n in tuple(names))
    return {fn(b,len(b),0):name for name in names if name.isascii() for b in [name.upper().encode()]}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("index",type=Path,help="output directory of frosty-shader global-index")
    p.add_argument("symbols",type=Path)
    args=p.parse_args();names=shader_names(args.symbols)
    b=(args.index/"global-cache.bin").read_bytes()
    def u(at):return struct.unpack_from("<I",b,at)[0]
    maps={}
    with (args.index/"global-maps.tsv").open() as f:
        for r in csv.DictReader(f,delimiter="\t"):
            h,ids=maps.setdefault(int(r["section"]),(bytes.fromhex(r["map_hash"]),[]))
            if len(ids)!=int(r["local_index"]) or h.hex()!=r["map_hash"]:raise ValueError("bad map table")
            ids.append((int(r["global_index"]),r["key"]))
    if u(0)!=len(maps):raise ValueError("global section count mismatch")
    at=4;resolved=total=0
    with (args.index/"types.tsv").open("w") as f:
        w=csv.writer(f,delimiter="\t");w.writerow(["section","global_index","key","permutation","type_hash","name"])
        for section in range(len(maps)):
            mh,ids=maps[section];align,version,size=struct.unpack_from("<III",b,at)
            if (align,version)!=(8,5):raise ValueError("unknown frozen shader map layout")
            start=at+12;end=start+size
            def ptr(at,n):
                raw=struct.unpack_from("<i",b,at)[0];target=at+(raw>>1)
                if not raw&1 or target<start or target+n>end:raise ValueError("bad frozen relative pointer")
                return target
            count=u(start+64)
            if u(start+32)!=count or u(start+48)!=count:raise ValueError("type/instance array size mismatch")
            table=ptr(start+24,8*count);perms=ptr(start+40,4*count);sh=ptr(start+56,8*count)
            used=set()
            for j in range(count):
                h=struct.unpack_from("<Q",b,table+8*j)[0];idx=u(ptr(sh+8*j,0xc8)+0xc4)
                if idx>=len(ids):raise ValueError("bad FShader resource index")
                used.add(idx);i,key=ids[idx];name=names.get(h,"")
                resolved+=bool(name);total+=1;w.writerow([section,i,key,u(perms+4*j),f"{h:016x}",name])
            # Some sections also contain pipelines whose shaders do not occur
            # in the independent type array. Never infer names by proximity.
            marker=b.find(mh,end)
            if marker<end:raise ValueError("section did not reference its packaged shader map")
            at=marker+len(mh)
    if at!=len(b):raise ValueError("trailing global shader cache data")
    print(f"{resolved}/{total} shader instances named from the shipped artifacts")


if __name__=="__main__":main()
