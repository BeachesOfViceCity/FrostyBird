"""Per-launch host compiler for the exact Xbox shader and bound numeric types."""
from contextlib import contextmanager, nullcontext
import json
import os
from pathlib import Path
import secrets
import socket
import select
from socketserver import BaseRequestHandler, ThreadingTCPServer
import struct
import subprocess
import csv
from threading import Lock, Semaphore, Thread
from shader_cache import CompileError, Request, ShaderCache, atomic_write, validate_header


def receive(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(min(65536, count-len(data)))
        if not part:
            raise EOFError
        data.extend(part)
    return bytes(data)

@contextmanager
def host_compiler(tool, cache, env, trace_dir=None):
    hold=env.get("FROSTY_HOLD_LIFTS")
    store=ShaderCache(tool,cache,hot=bool(hold))
    # An isolated compute compiler trial can share the already prepared
    # graphics archive. Both executables have independently validated shards.
    compute=ShaderCache(env["FROSTY_COMPUTE_TOOL"],cache,hot=bool(hold)) if env.get("FROSTY_COMPUTE_TOOL") else store
    if hold:
        hold=Path(hold).resolve()
        hold.mkdir(parents=True,exist_ok=True)
    def repair(req,error):
        # Pause a diagnostic launch on its actual request. Only the normal
        # compiler can produce its response; the FIFO is just a retry signal.
        raw,stage,caps=req.raw,req.wire&255,req.wire>>8
        prefix=hold/req.name
        raw_path=Path(str(prefix)+(".xsrt" if stage in (6,7,8) else ".dxbc"));raw_path.write_bytes(raw)
        fifo=Path(str(prefix)+".fifo")
        fifo.unlink(missing_ok=True);os.mkfifo(fifo,0o600)
        fd=os.open(fifo,os.O_RDWR|os.O_NONBLOCK)
        record=Path(str(prefix)+".pending.json")
        try:
            key=0xcbf29ce484222325
            for b in raw:key=(key*0x100000001b3&0xffffffffffffffff)^b
            msg={"key":f"{key:016x}","stage":stage,"caps":caps,"views":req.spec,"dxbc":str(raw_path),"reason":error,"retry":str(fifo),
                 "params_hex":req.params.hex(),"cache_dir":str(store.root)}
            tmp=Path(str(record)+".tmp");tmp.write_text(json.dumps(msg)+"\n");tmp.replace(record)
            print(f"frosty/lift-stop: {key:016x} {error}",flush=True)
            return bool(select.select([fd],[],[],870)[0]) and os.read(fd,64).strip()==b"retry"
        finally:
            os.close(fd);fifo.unlink(missing_ok=True);record.unlink(missing_ok=True)
    token = secrets.token_hex(16)
    slots = Semaphore(min(6, os.cpu_count() or 1))
    locks = [Lock() for _ in range(64)]
    failures = {}
    failed_dir=Path(env["XS_DEBUG_FAILED_SHADERS"]) if env.get("XS_DEBUG_FAILED_SHADERS") else None
    recorded=set();record_lock=Lock();finished=False;xsrt=None

    def report():
        stats=store.summary()
        if compute is not store:
            for k,v in compute.summary().items():stats[k]=stats.get(k,0)+v
        stats["unique_requests"]=len(recorded)
        if xsrt is not None:stats["xsrt_sources"]=xsrt
        if trace_dir:atomic_write(trace_dir/"shader-cache.json",(json.dumps(stats,indent=2)+"\n").encode())
        return stats

    class Handler(BaseRequestHandler):
        def handle(self):
            sock = self.request
            sock.settimeout(120)
            try:
                if receive(sock, 32) != token.encode():
                    return
                wire, size, n = struct.unpack("<III", receive(sock, 12))
                validate_header(wire,size,n)
                types = receive(sock, n)
                raw = receive(sock, size)
                req=Request.make(raw,wire,types)
                compiler=compute if (wire&255)==5 else store
                data=compiler.lookup(req,compiler.snapshot())
                if data is None:
                    with locks[int(req.digest[:2], 16) % len(locks)]:
                        while True:
                            snap=compiler.snapshot();path=snap.directory/req.name
                            if path in failures:raise ValueError(failures[path])
                            try:
                                with slots:data=compiler.resolve(req,snap,timeout=900 if hold else 120)
                            except CompileError as err:
                                if failed_dir:
                                    failed_dir.mkdir(parents=True,exist_ok=True)
                                    atomic_write(failed_dir/(req.digest+".dxbc"),raw)
                                    atomic_write(failed_dir/(req.name+".json"),json.dumps(dict(
                                        wire=wire,parameters=types.hex(),error=str(err),exit=err.code)).encode())
                                if hold and repair(req,str(err)):continue
                                failures[path]=str(err)
                                raise
                            break
                with record_lock:
                    if not finished and req.name not in recorded:
                        store.remember(req)
                        if manifest:
                            dst=trace_dir/"shader-inputs"/req.digest
                            try:os.link(store.inputs/req.digest,dst)
                            except FileExistsError:
                                if not dst.samefile(store.inputs/req.digest) and dst.read_bytes()!=raw:atomic_write(dst,raw)
                            except OSError:atomic_write(dst,raw)  # e.g. different filesystems
                            manifest.write(json.dumps({"sha256":req.digest,"wire":wire,"params":req.params.hex()})+"\n")
                            manifest.flush()
                        recorded.add(req.name)
                        if trace_dir and (len(recorded)==1 or len(recorded)%128==0):report()
                sock.sendall(struct.pack("<I", 0) + data)
            except (EOFError, ConnectionError, socket.timeout):
                return
            except (OSError, ValueError, subprocess.SubprocessError) as e:
                msg = str(e).encode()[:2048]
                try:
                    sock.sendall(struct.pack("<II", 1, len(msg)) + msg)
                except OSError:
                    pass

    class Server(ThreadingTCPServer):
        daemon_threads = True
        request_queue_size = 64
        allow_reuse_address = True

    if trace_dir:(trace_dir/"shader-inputs").mkdir(exist_ok=True)
    manifest_ctx=(trace_dir/"shader-requests.jsonl").open("w") if trace_dir else nullcontext()
    timing=env.get("FROSTY_HOST_COMPILE_TIMES")
    with manifest_ctx as manifest, (Path(timing).open("x",buffering=1) if timing else nullcontext()) as times, Server(("127.0.0.1", 0), Handler) as server:
        if times:
            clock_lock=Lock()
            rows=csv.writer(times,delimiter="\t",lineterminator="\n");rows.writerow(("begin_us","end_us","request","returncode","dxc"))
            def event(req,begin,end,code,dxc):
                with clock_lock:
                    if not times.closed:rows.writerow((begin,end,req.name,code,int(dxc)))
            store.observe_compile=compute.observe_compile=event
        env["FROSTY_COMPILER_PORT"] = str(server.server_address[1])
        env["FROSTY_COMPILER_TOKEN"] = token
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield
        finally:
            server.shutdown()
            thread.join()
            with record_lock:
                finished=True
                if trace_dir:xsrt=store.retain_xsrt(trace_dir,compact=True)
                print("frosty/shader-cache: "+json.dumps(report(),sort_keys=True),flush=True)
