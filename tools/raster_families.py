"""Train exact native states for two-sided material variants of cooked VS/PS pairs."""
from collections import Counter,defaultdict
import hashlib
import json
import struct
import time

from pipeline_families import split_graphics
from shader_cache import ShaderCache,Request,atomic_write

def prepare(tool,cache,report=None,*,before=None,preview=False):
    start=time.monotonic();store=ShaderCache(tool,cache);root=store.root/"pipeline-recipes-v1"
    # XSP1 stores every fixed-state field without native struct padding.
    # Hold the root, attachment formats, primitive type and sample state
    # fixed when transferring a cull mode from another use of the same PS.
    sources=[];modes=defaultdict(set);vs_types=defaultdict(Counter);ps_types=defaultdict(Counter)
    layouts={};prior=defaultdict(set);known=set()
    def layout(raw,stage):
        h=hashlib.sha256(raw).digest()
        if h not in layouts:
            p=store.lookup(Request.make(raw,stage),store.initial)
            layouts[h]=None if p is None else struct.unpack_from("<I",p)[0]
        return h,layouts[h]
    for folder in (root,root/"predicted",root/"training"):
        hint_files=defaultdict(list)
        for hint in folder.glob("*.hint"):
            parts=hint.name.split(".")
            if len(parts)==3 and (before is None or hint.stat().st_mtime<before):hint_files[parts[0]].append((parts[1],hint))
        for f in folder.glob("*.plan"):
            if before is not None and f.stat().st_mtime>=before:continue
            b=f.read_bytes()
            if hashlib.sha256(b).hexdigest()!=f.stem:continue
            g=split_graphics(b)
            if not g or not g[1] or not g[2]:continue
            pre,vs,ps,post=g;off=12+struct.unpack_from("<I",pre,8)[0]
            n=struct.unpack_from("<I",pre,off+32)[0];end=off+36+4*(n+1);pos=end+8+37*8+4
            if n>8 or pos+4>len(pre):continue
            cull=struct.unpack_from("<I",pre,pos)[0]
            if cull not in (1,2,3):continue
            vh,nv=layout(vs,1);ph,np=layout(ps,0);family=hashlib.sha256(pre[:end]).digest(),ph
            known.add(f.stem)
            for digest,hint in hint_files[f.stem]:
                h=hint.read_bytes()
                if not h or len(h)>512 or hashlib.sha256(h).hexdigest()!=digest:continue
                prior[f.stem].add(h)
                if folder==root and nv is not None and np is not None and len(h)==nv+np:
                    vs_types[vh][h[:nv]]+=1;ps_types[ph][h[nv:]]+=1
            if folder==root:modes[family].add(cull)
            sources.append((f.stem,b,pos,cull,family,vh,nv,ph,np))
    out=root/"raster-training";live=root/"predicted"
    if not preview:out.mkdir(exist_ok=True);live.mkdir(exist_ok=True)
    selected=set();new_states=set();new_hints=set()
    for original,b,pos,cull,family,vh,nv,ph,np in sources:
        if family not in modes:continue
        hints=prior[original].copy()
        if nv is not None and np is not None:
            left=vs_types[vh].most_common(3) or [(bytes(nv),1)]
            right=ps_types[ph].most_common(3) or [(bytes(np),1)]
            hints.update(x+y for x,_ in left for y,_ in right if any(x+y))
        for mode in modes[family]|{cull}:
            data=b if mode==cull else b[:pos]+struct.pack("<I",mode)+b[pos+4:]
            key=original if mode==cull else hashlib.sha256(data).hexdigest()
            extra=hints-prior.get(key,set())
            if key in known and not extra:continue
            selected.add(key)
            if key not in known:new_states.add(key)
            file=out/(key+".plan")
            if not preview and not file.exists():atomic_write(file,data)
            if not preview and key not in known and not (live/file.name).exists():
                atomic_write(live/file.name,data)
            for h in extra:
                dg=hashlib.sha256(h).hexdigest();new_hints.add((key,dg))
                path=out/(key+"."+dg+".hint")
                if not preview and not path.exists():atomic_write(path,h)
                if not preview and ((live/file.name).exists() or (root/file.name).exists()) and not (live/path.name).exists():
                    atomic_write(live/path.name,h)
    result=dict(source_recipes=len(sources),training_recipes=len(selected),new_fixed_states=len(new_states),
                new_bound_variants=len(new_hints),elapsed_s=round(time.monotonic()-start,3))
    if preview:result["selected"]=sorted(selected);result["bound"]=[list(h) for h in sorted(new_hints)]
    if report:atomic_write(report,(json.dumps(result,indent=2)+"\n").encode())
    print("Raster families:",{k:v for k,v in result.items() if k not in ("selected","bound")},flush=True)
    return result
