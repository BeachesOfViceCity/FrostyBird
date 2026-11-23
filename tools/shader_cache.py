"""Persistent shader packets and the exact requests needed to prewarm another compiler build."""
from collections import Counter
from dataclasses import dataclass
import fcntl
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import struct
import subprocess
import time
from threading import Lock

MAX_CODE = 16 << 20
STAGES = (0, 1, 2, 5, 6, 7, 8, 129)


def atomic_write(path, data):
    tmp = path.with_name("." + path.name + "." + secrets.token_hex(8) + ".tmp")
    try:
        tmp.write_bytes(data)
        tmp.replace(path)
    finally:
        tmp.unlink(missing_ok=True)


def read_bounded(path, limit):
    with path.open("rb") as f:
        size=os.fstat(f.fileno()).st_size
        if size>limit:raise ValueError(f"oversized cache input: {path}")
        data=f.read(size+1)
    if len(data)!=size:raise ValueError(f"cache input changed while reading: {path}")
    return data


def validate_header(wire, size, n):
    stage, caps = wire & 255, wire >> 8
    if (stage not in STAGES or not 0 <= caps <= 15 or (caps and stage != 5)
            or not 32 <= size <= MAX_CODE or not 0 <= n <= (8192 if stage == 6 else 256)):
        raise ValueError("invalid shader request header")


@dataclass(frozen=True)
class Request:
    raw: bytes
    wire: int
    params: bytes
    digest: str
    name: str

    @classmethod
    def make(cls, raw, wire, params=b""):
        validate_header(wire, len(raw), len(params))
        # A probe and the all-default numeric specialization compile identically.
        if (wire & 255) not in (6, 7, 8) and not any(params):
            params = b""
        digest = hashlib.sha256(raw).hexdigest()
        name = f"{digest}.{wire}.{hashlib.sha256(params).hexdigest()}"
        return cls(raw, wire, params, digest, name)

    @property
    def spec(self):
        if any(b > 2 for b in self.params):
            return "x" + self.params.hex()
        return "".join(map(str, self.params)) or "-"

    def arguments(self):
        stage, caps = self.wire & 255, self.wire >> 8
        if stage == 6:
            if len(self.params) <= 16:raise ValueError("short raygen view contract")
            words, route, field, value = struct.unpack_from("<4I", self.params)
            layout = self.params[16:].decode("ascii")
            form = r"\d{1,3}:\d{1,2}:(?:float|uint|int|raw|\d{1,4})"
            if not field < words <= 32 or route > 0xffffff or not re.fullmatch(form+"(?:,"+form+")*", layout):
                raise ValueError("invalid raygen view contract")
            return ["rt-rg-packet", *map(str, (words, route, field, value)), layout]
        if stage in (7, 8):
            if len(self.params) < 13:raise ValueError("short ray payload contract")
            words, field, value = struct.unpack_from("<3I", self.params)
            symbol = self.params[12:].decode("ascii")
            if not field < words <= 32 or not re.fullmatch("[A-Za-z_][A-Za-z_0-9]*", symbol):
                raise ValueError("invalid ray payload/export")
            return ["rt-hit-packet" if stage == 8 else "rt-miss-packet", *map(str, (words, field, value)), symbol]
        return ["specialize", "rectlist" if stage == 129 else str(stage), self.spec] + ([str(caps)] if caps else [])


def valid_packet(data):
    if len(data) < 8:return False
    n, length = struct.unpack_from("<II", data)
    off = 8 + 4*n
    return (n <= 256 and 32 <= length <= MAX_CODE and len(data) == off + length
            and data[off:off+4] == b"DXBC" and struct.unpack_from("<I", data, off+24)[0] == length)


def toolchain(executable, digest):
    deps = subprocess.check_output(["ldd", str(executable)], text=True, timeout=10)
    if "=> not found" in deps:raise OSError("unresolved shader compiler library")
    components = {"translator": digest}
    stamps = {}
    def fingerprint(soname,path):
        path=Path(path)
        before=file_stamp(path)
        with path.open("rb") as f:
            components[soname] = hashlib.file_digest(f, "sha256").hexdigest()
        if file_stamp(path)!=before:raise OSError("compiler library changed while reading")
        stamps[path]=before
    for soname,path in re.findall(r"^\s*(lib(?:dxcompiler|LLVM)[^\s]*)\s+=>\s+(.+?)\s+\(0x",deps,re.M):
        fingerprint(soname,path)
        if soname.startswith("libdxcompiler"):
            # DXC dlopens its validator; it isn't reported by ordinary ldd.
            # Cover both the distribution's adjacent library (DXC's own
            # rpath) and a different system/override resolution. Trace mode
            # only asks the ELF loader for paths; no shader is compiled.
            sibling=Path(path).resolve().with_name("libdxil.so")
            preload=" ".join(filter(None,(os.environ.get("LD_PRELOAD"),"libdxil.so")))
            probe=subprocess.run([executable],capture_output=True,text=True,timeout=10,check=True,
                                 env={**os.environ,"LD_TRACE_LOADED_OBJECTS":"1","LD_PRELOAD":preload})
            match=re.search(r"^\s*libdxil\.so\s+=>\s+(.+?)\s+\(0x",probe.stdout,re.M)
            if sibling.is_file():fingerprint("libdxil.so",sibling)
            else:stamps[sibling]=None
            if match and (not sibling.exists() or not sibling.samefile(match[1])):
                fingerprint("libdxil.so:loader",match[1])
    return components,stamps


def file_stamp(path):
    s=path.stat()
    return s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_mode


@dataclass
class Snapshot:
    executable: Path
    directory: Path
    env: dict
    libraries: dict

    def libraries_current(self):
        return all((not p.exists() if stamp is None else file_stamp(p)==stamp) for p,stamp in self.libraries.items())


class CompilerPublisher:
    """Pin an executable while the development launcher can be relinking it."""
    def __init__(self, tool, root):
        self.tool, self.root = Path(tool).resolve(), root
        self.lock = Lock()
        self.stamp = self.current = None

    def get(self):
        with self.lock:
            try:
                before = file_stamp(self.tool)
                if before == self.stamp and self.current.libraries_current():return self.current
                if not before[4] & 0o111:raise OSError("compiler is being linked")
                data = self.tool.read_bytes()
                if file_stamp(self.tool) != before or len(data) != before[2]:raise OSError("compiler changed while reading")
                digest = hashlib.sha256(data).hexdigest()
                exe = self.tool.with_name("." + self.tool.name + "." + digest[:20])
                if not exe.is_file() or exe.read_bytes()!=data:
                    tmp = exe.with_suffix("." + secrets.token_hex(8) + ".publishing")
                    try:
                        tmp.write_bytes(data);tmp.chmod(0o755)
                        check = subprocess.run([tmp], capture_output=True, timeout=10)
                        if check.returncode != 2 or b"frosty-shader devices" not in check.stderr:
                            raise OSError("incomplete compiler")
                        tmp.replace(exe)
                    finally:
                        tmp.unlink(missing_ok=True)
                ids,libs = toolchain(exe, digest)
                info = json.dumps(ids, sort_keys=True).encode()
                directory = self.root/"packets-v1"/hashlib.sha256(info).hexdigest()
                directory.mkdir(parents=True, exist_ok=True)
                if not (directory/"toolchain.json").exists():atomic_write(directory/"toolchain.json", info+b"\n")
                dxc_ids={k:v for k,v in ids.items() if k.startswith("libdx")}
                dxc=hashlib.sha256(json.dumps(dxc_ids,sort_keys=True).encode()).hexdigest() if dxc_ids else digest
                sources = self.root/"by-source"/dxc
                sources.mkdir(parents=True, exist_ok=True)
                env = {**os.environ, "FROSTY_DXIL_SOURCE_CACHE": str(sources), "FROSTY_DXC_CACHE_REPORT": "1"}
                self.current = Snapshot(exe, directory, env, libs)
                self.stamp = before
            except (OSError, subprocess.SubprocessError):
                if self.current is None or not self.current.libraries_current():raise
            return self.current


class CompileError(ValueError):
    def __init__(self, text, code):
        super().__init__(text)
        self.code = code


class ShaderCache:
    def __init__(self, tool, root, hot=False):
        self.root = Path(root).expanduser().resolve()
        self.publisher = CompilerPublisher(tool, self.root)
        self.initial = self.publisher.get()
        self.hot = hot
        self.inputs = self.root/"workload-v1"/"inputs"
        self.recipes = self.inputs.parent/"recipes"
        self.locks = self.root/"locks"
        for d in (self.inputs, self.recipes, self.locks):d.mkdir(parents=True, exist_ok=True)
        self.stats = Counter()
        self.observe_compile = None
        self.mutex = Lock()

    def snapshot(self):
        return self.publisher.get() if self.hot else self.initial

    def count(self, key):
        with self.mutex:self.stats[key] += 1

    def remember(self, r):
        for path,data in ((self.inputs/r.digest,r.raw),(self.recipes/r.name,r.params)):
            try:
                if path.stat().st_size==len(data) and path.read_bytes()==data:continue
            except FileNotFoundError:pass
            atomic_write(path,data)

    def retain_xsrt(self, capture, *, compact=False):
        """Keep observed original collections, even ones served by prepared DXIL.

        Keeping the bytes does not create a ray compilation recipe. Only a
        real request supplies its export, payload and binding layout.
        """
        counts={"originals":0,"already_stored":0,"shared_files":0,"invalid":0}
        for path in Path(capture).glob("*.xsrt"):
            if not re.fullmatch("[0-9a-fA-F]{16}",path.stem):continue
            with path.open("rb") as f:
                size=os.fstat(f.fileno()).st_size
                if not 20<=size<=(64<<20) or f.read(4)!=b"\x02\x00\x00\x00":
                    counts["invalid"]+=1;continue
                f.seek(0);raw=f.read()
            # A title killed mid-capture can leave a partial CREATE_NEW file.
            # The guest filename uses FNV-1; verify it before deduplicating
            # under a full SHA-256 content address.
            key=0xcbf29ce484222325
            for byte in raw:key=((key*0x100000001b3)&0xffffffffffffffff)^byte
            if len(raw)!=size or key!=int(path.stem,16):
                counts["invalid"]+=1;continue
            target=self.inputs/hashlib.sha256(raw).hexdigest()
            counts["originals"]+=1
            try:
                os.link(path,target)
            except FileExistsError:
                if target.samefile(path):
                    counts["already_stored"]+=1;counts["shared_files"]+=1;continue
                if target.stat().st_size==size and target.read_bytes()==raw:counts["already_stored"]+=1
                else:atomic_write(target,raw)
                if not compact:continue  # importing someone else's old capture is read-only
                tmp=path.with_name("."+path.name+"."+secrets.token_hex(8)+".tmp")
                try:
                    os.link(target,tmp);tmp.replace(path)
                except OSError as err:
                    if err.errno not in (errno.EXDEV,errno.EPERM,errno.EOPNOTSUPP,errno.EMLINK):raise
                    continue
                finally:tmp.unlink(missing_ok=True)
            except OSError as err:
                if err.errno not in (errno.EXDEV,errno.EPERM,errno.EOPNOTSUPP,errno.EMLINK):raise
                if not target.is_file() or target.read_bytes()!=raw:atomic_write(target,raw)
                else:counts["already_stored"]+=1
                continue
            counts["shared_files"]+=1
        return counts

    def recorded(self, name):
        if not re.fullmatch(r"[0-9a-f]{64}\.\d{1,4}\.[0-9a-f]{64}", name):
            raise ValueError(f"invalid recorded shader ID: {name!r}")
        digest, wire, _ = name.split(".")
        r = Request.make(read_bounded(self.inputs/digest,MAX_CODE),int(wire),read_bounded(self.recipes/name,8192))
        if r.name != name:raise ValueError(f"recorded input or parameters do not match {name}")
        return r

    @staticmethod
    def _read_packet(path,prefix):
        try:
            with path.open("rb") as f:
                if os.fstat(f.fileno()).st_size>32+8+4*256+MAX_CODE:return None
                tag,packet=f.read(32),f.read()
        except FileNotFoundError:return None
        if valid_packet(packet) and hashlib.sha256(prefix+packet).digest()==tag:return packet
        return None

    def lookup(self,r,snap):
        """An intact published packet never waits behind a compiler worker."""
        r.arguments()
        packet=self._read_packet(snap.directory/r.name,(snap.directory.name+":"+r.name).encode())
        if packet is not None:
            self.count("requests");self.count("disk_hits")
        return packet

    def resolve(self, r, snap=None, timeout=900):
        args = r.arguments()
        snap = snap or self.snapshot()
        path = snap.directory/r.name
        # Bind the integrity check to the original bytes, wire flags, params
        # and toolchain as well as the returned view list and DXIL.
        prefix = (snap.directory.name + ":" + r.name).encode()
        self.count("requests")
        packet = self._read_packet(path,prefix)
        if packet is not None:
            self.count("disk_hits");return packet
        # Racing launchers and offline warmers share the same publications.
        # Only a miss takes a lock. Recheck after the producing process exits.
        with (self.locks/r.digest[:2]).open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            packet = self._read_packet(path,prefix)
            if packet is not None:
                self.count("disk_hits");return packet
            if path.exists():self.count("invalid_packets")
            if not snap.libraries_current():raise OSError("shader toolchain changed before compilation")
            self.count("translations")
            if self.observe_compile:begin=time.time_ns()//1000
            try:
                proc = subprocess.run([snap.executable, *args], input=r.raw, capture_output=True, timeout=timeout, env=snap.env)
            except subprocess.TimeoutExpired as err:
                if self.observe_compile:self.observe_compile(r,begin,time.time_ns()//1000,124,False)
                self.count("failed")
                raise CompileError(f"shader compiler timed out after {timeout}s",124) from err
            if self.observe_compile:self.observe_compile(r,begin,time.time_ns()//1000,proc.returncode,b"frosty/dxc-cache: compile" in proc.stderr.splitlines())
            for line in proc.stderr.splitlines():
                if line == b"frosty/dxc-cache: hit":self.count("dxc_hits")
                elif line == b"frosty/dxc-cache: compile":self.count("dxc_compiles")
            if proc.returncode:
                self.count("failed")
                error = proc.stderr.decode(errors="replace").strip() or f"compiler exited {proc.returncode}"
                raise CompileError(error[-2000:], proc.returncode)
            if not valid_packet(proc.stdout):raise ValueError("invalid compiler packet")
            if not snap.libraries_current():raise OSError("shader toolchain changed during compilation")
            atomic_write(path, hashlib.sha256(prefix+proc.stdout).digest()+proc.stdout)
            return proc.stdout

    def summary(self):
        with self.mutex:
            return {k:self.stats[k] for k in ("requests","disk_hits","translations","dxc_hits","dxc_compiles","invalid_packets","failed")}
