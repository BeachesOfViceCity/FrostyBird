"""Offline replay of shader requests through the same cache used by the Wine launcher."""
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
import json
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

from shader_cache import CompileError, MAX_CODE, Request, ShaderCache, atomic_write, read_bounded


def capture_manifest(path):
    path=Path(path)
    if path.is_dir():
        candidate=path/"shader-requests.jsonl"
        return candidate if candidate.is_file() else None
    return path if path.suffix==".jsonl" else None


def manifest_requests(store, path):
    with path.open() as file:
        for lineno, line in enumerate(file, 1):
            try:
                row=json.loads(line)
                digest,wire,params=row["sha256"],row["wire"],row["params"]
                if (not isinstance(digest,str) or not re.fullmatch("[0-9a-f]{64}",digest)
                        or type(wire) is not int or not isinstance(params,str) or len(params)>16384):
                    raise ValueError("invalid shader request")
                src=path.parent/"shader-inputs"/digest
                if not src.is_file():src=store.inputs/digest
                r=Request.make(read_bounded(src,MAX_CODE),wire,bytes.fromhex(params))
                if r.digest!=digest:raise ValueError("original shader digest mismatch")
                yield r
            except (ValueError,KeyError,TypeError,OSError) as err:
                raise ValueError(f"{path}:{lineno}: {err}") from err


def trace_stages(path):
    stages={"ps":0,"vs":1,"gs":2,"cs":5}
    pattern=re.compile(r"frosty/compile-begin:\s+\w+\s+(ps|vs|gs|cs)\s+([0-9a-fA-F]{16})\b")
    with path.open(errors="replace") as log:
        keys={(stages[m[1]],int(m[2],16)) for line in log if (m:=pattern.search(line))}
    if not keys:raise ValueError(f"no traced stage requests in {path}; use a capture or --archive")
    return keys


def archive_requests(store, pak, selected, wave_caps, accept=None, with_keys=False):
    # One package scan; bound memory even for the full cook. A trace's short
    # key is just an index: the actual cache always hashes the complete bytes.
    caps=range(16) if wave_caps=="all" else (int(wave_caps),)
    cmd=[store.initial.executable,"cache-inputs",pak]
    matched={}
    with tempfile.TemporaryFile() as keys:
        if selected is not None:
            cmd.append("--selected")
            for stage,key in sorted(selected):keys.write(f"{stage} {key:016x}\n".encode())
            keys.seek(0)
        with subprocess.Popen(cmd,stdin=keys,stdout=subprocess.PIPE) as proc:
            try:
                while header:=proc.stdout.read(16):
                    if len(header)!=16:raise ValueError("short package shader header")
                    key,stage,size=struct.unpack("<QII",header)
                    if stage not in (0,1,2,5) or not 32<=size<=MAX_CODE:raise ValueError("invalid package shader header")
                    raw=proc.stdout.read(size)
                    if len(raw)!=size:raise ValueError("short package shader")
                    if accept is not None and not accept(stage,key):continue
                    for c in caps if stage==5 else (0,):
                        r=Request.make(raw,stage|(c<<8))
                        if selected is not None and matched.setdefault((stage,key),r.digest)!=r.digest:
                            raise ValueError(f"ambiguous package shader key {key:016x}")
                        yield (key,r) if with_keys else r
                if proc.wait():raise ValueError("could not resolve package shader inputs")
            finally:
                proc.stdout.close()
                if proc.poll() is None:proc.terminate()

def pixel_linkage(raw):
    """The cooked input/output signatures select *candidates*, never cache keys."""
    if len(raw)<32 or raw[:4]!=b"DXBC":return None
    n,=struct.unpack_from("<I",raw,28)
    if n>(len(raw)-32)//4:return None
    parts={}
    for (at,) in struct.iter_unpack("<I",raw[32:32+4*n]):
        if at+8>len(raw):return None
        tag=raw[at:at+4];length,=struct.unpack_from("<I",raw,at+4)
        if length>len(raw)-at-8:return None
        if tag in (b"ISG1",b"OSG1"):parts[tag]=raw[at:at+8+length]
    if len(parts)!=2:return None
    return hashlib.sha256(parts[b"ISG1"]+parts[b"OSG1"]).digest()

def prewarm_materials(tool,root,pak,jobs=4,report=None):
    """Cook unplayed material permutations of observed stage linkage ahead of a title run.

    Candidate discovery may overpredict. Each packet is still translated
    from its own complete original, with its own exact view vector.
    """
    if not pak or not 1<=jobs<=32:raise ValueError("material prewarm needs a package and 1–32 jobs")
    store=ShaderCache(tool,root);families={};seen=set();errors=[];unsupported=variants=0
    started=time.monotonic()
    def bindings(packet):
        n=struct.unpack_from("<I",packet)[0];return struct.unpack_from("<"+"I"*n,packet,8)
    def plain(slots):
        return all(not (slot&~0x3ff) and not (slot&0x80) for slot in slots)
    for f in store.recipes.glob("*.0.*"):
        if not f.stat().st_size:continue
        req=store.recorded(f.name);key=pixel_linkage(req.raw)
        if key is None:continue
        slots=bindings(store.resolve(req));exact,uniform=families.setdefault(key,({},set()))
        exact.setdefault(slots,set()).add(req.params)
        if plain(slots) and len(set(req.params))==1 and req.params[0] in (1,2):uniform.add(req.params[0])
    def warm(req,model):
        try:
            slots=bindings(store.resolve(req));exact,uniform=model
            values=set(exact.get(slots,()))
            if slots and plain(slots):values.update(bytes([t])*len(slots) for t in uniform)
            for v in sorted(values):store.resolve(Request.make(req.raw,0,v))
            return len(values),False,None
        except CompileError as e:return 0,e.code==3,None if e.code==3 else {"key":req.name,"reason":str(e)}
    def collect(done):
        nonlocal variants,unsupported
        for task in done:
            n,skip,err=task.result();variants+=n;unsupported+=skip
            if err:errors.append(err)
    with ThreadPoolExecutor(jobs) as pool:
        pending=set()
        for req in archive_requests(store,pak,None,"0"):
            if req.wire!=0 or req.digest in seen:continue
            model=families.get(pixel_linkage(req.raw))
            if not model:continue
            seen.add(req.digest);pending.add(pool.submit(warm,req,model))
            if len(pending)>=2*jobs:
                done,pending=wait(pending,return_when=FIRST_COMPLETED);collect(done)
        collect(pending)
    from pipeline_families import prepare
    predicted=prepare(store,archive_requests(store,pak,None,"0"))
    result={"mode":"material families","linkage_families":len(families),"candidate_sources":len(seen),"typed_variants":variants,
            "unsupported_sources":unsupported,"predicted_pipelines":predicted,"errors":errors,**store.summary(),"elapsed_seconds":round(time.monotonic()-started,3)}
    if report:atomic_write(report,(json.dumps(result,indent=2)+"\n").encode())
    print("Material prewarm:",json.dumps({k:v for k,v in result.items() if k!="errors"}),f"({len(errors)} errors)",flush=True)
    return result


def prewarm(tool, root, *, pak=None, archive=False, from_run=None, jobs=6, wave_caps="all", report=None):
    if not 1<=jobs<=32:raise ValueError("--jobs must be between 1 and 32")
    if str(wave_caps) not in ("all",*map(str,range(16))):raise ValueError("invalid compute capability mask")
    start=time.monotonic()
    store=ShaderCache(tool,root)
    mode="archive" if archive else "recorded"
    manifest=None;selected=None
    if from_run is not None:
        path=Path(from_run)
        manifest=capture_manifest(path)
        if manifest is not None:mode="capture"
        else:
            selected=trace_stages(path/"launch.log" if path.is_dir() else path)
            mode="legacy trace (base stages only)"
    if (archive or selected is not None) and pak is None:raise ValueError("this mode requires --game or FROSTY_GAME_ROOT")

    skipped=0;errors=[];seen=set()
    def inputs():
        if mode=="capture":
            for r in manifest_requests(store,manifest):yield r,False,True
        else:
            if selected is None:
                for path in sorted(store.recipes.iterdir()):
                    if not path.name.startswith("."):
                        r=store.recorded(path.name);yield r,False,False
                        if r.wire&255==5 and str(wave_caps)!="all":
                            yield Request.make(r.raw,5|(int(wave_caps)<<8),r.params),False,False
            if archive or selected is not None:
                for r in archive_requests(store,pak,selected,str(wave_caps)):yield r,archive,selected is not None

    skipped_dir=store.initial.directory/"unsupported"
    if archive:skipped_dir.mkdir(exist_ok=True)
    def one(r,optional,remember):
        marker=skipped_dir/r.name
        if optional and marker.is_file():return True,None
        try:store.resolve(r)
        except CompileError as err:
            if optional and err.code==3:
                atomic_write(marker,(str(err)+"\n").encode());return True,None
            return False,{"request":r.name,"reason":str(err)}
        if remember:store.remember(r)
        return False,None

    def collect(done):
        nonlocal skipped
        for task in done:
            unsupported,error=task.result()
            skipped+=unsupported
            if error:errors.append(error)

    with ThreadPoolExecutor(jobs) as pool:
        pending=set()
        for r,optional,remember in inputs():
            if r.name in seen:continue
            seen.add(r.name)
            pending.add(pool.submit(one,r,optional,remember))
            if len(pending)>=2*jobs:
                done,pending=wait(pending,return_when=FIRST_COMPLETED);collect(done)
        collect(pending)
    result={"mode":mode,"workload":len(seen),"unsupported":skipped,**store.summary(),"errors":errors,
            "toolchain":store.initial.directory.name}
    if from_run is not None and Path(from_run).is_dir():result["xsrt_sources"]=store.retain_xsrt(from_run)
    result["elapsed_seconds"]=round(time.monotonic()-start,3)
    if report:
        path=Path(report);path.parent.mkdir(parents=True,exist_ok=True)
        atomic_write(path,(json.dumps(result,indent=2)+"\n").encode())
    print(f"Prewarm ({mode}): {len(seen)} variants, {result['disk_hits']} packet hits, "
          f"{result['translations']} translator runs, {result['dxc_compiles']} DXC compiles, "
          f"{result['dxc_hits']} DXC cache hits, {skipped} unsupported, {len(errors)} errors.",flush=True)
    sources=result.get("xsrt_sources")
    if sources and (sources["originals"] or sources["invalid"]):
        print(f"XSRT originals: {sources['originals']} retained, {sources['already_stored']} already cached, "
              f"{sources['invalid']} incomplete or mismatched.",flush=True)
    if errors:print("First error:",errors[0]["reason"],flush=True)
    if not seen:print("No recorded shaders yet. Run the title once, or seed from its package with --archive.",flush=True)
    return result
