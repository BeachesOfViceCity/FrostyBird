"""Cache identity, publication and replay tests; GPU conformance uses the retail clear/draw programs."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import socket
import struct
import sys
import tempfile
from threading import Event
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from prewarm_shaders import prewarm,prewarm_materials
from shader_cache import CompileError, Request, ShaderCache
from shader_service import host_compiler, receive
from pipeline_families import prepare as prepare_pipelines,split_graphics
import hashlib


# A counting subprocess instead of a GPU compiler: all inputs appear in the
# result, so aliasing even one view, capability bit or ray parameter is visible.
COMPILER = """#!/usr/bin/env python3
import hashlib,struct,sys
from pathlib import Path
root=Path(__file__).parent
if len(sys.argv)==1:
    print("frosty-shader devices",file=sys.stderr);sys.exit(2)
with (root/"invocations").open("a") as f:f.write(" ".join(sys.argv[1:])+"\\n")
if (root/"fail-once").exists():
    (root/"fail-once").unlink();print("expected test failure",file=sys.stderr);sys.exit(1)
tag=hashlib.sha256(sys.stdin.buffer.read()+repr(sys.argv[1:]).encode()).digest()
obj=b'DXBC'+tag[:16]+struct.pack('<III',1,64,0)+tag
sys.stdout.buffer.write(struct.pack('<III',1,len(obj),0)+obj)
"""


class ShaderCacheTest(unittest.TestCase):
    def setUp(self):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup)
        self.root=Path(tmp.name);self.tool=self.root/"frosty-shader"
        self.tool.write_text(COMPILER);self.tool.chmod(0o755)
        self.library="0"*64
        mocked=patch("shader_cache.toolchain",side_effect=lambda exe,digest:
                     ({"translator":digest,"libdxcompiler.so":self.library},{}))
        mocked.start();self.addCleanup(mocked.stop)
        self.raw=b"source A"*8

    def calls(self):
        return (self.root/"invocations").read_text().splitlines()

    def send(self,env,wire,params):
        with socket.create_connection(("127.0.0.1",int(env["FROSTY_COMPILER_PORT"])),timeout=10) as s:
            s.sendall(env["FROSTY_COMPILER_TOKEN"].encode()+struct.pack("<III",wire,len(self.raw),len(params))+params+self.raw)
            status,n=struct.unpack("<II",receive(s,8));self.assertEqual(status,0)
            size,=struct.unpack("<I",receive(s,4))
            return receive(s,4*n+size)

    def test_capture_prewarms_a_fresh_cache_including_ray_contracts(self):
        env={};capture=self.root/"capture";capture.mkdir()
        words=struct.pack("<4I",16,33,9,0x800000)
        variants=[(773,b""),(773,b"\0"),(773,b"\1\2"),(261,b"\1\2"),(129,b""),
                  (6,words+b"5:4:uint,10:11:float"),(6,words+b"5:4:float,10:11:float"),
                  (8,struct.pack("<3I",16,9,0x800000)+b"MaterialCHS_01")]
        with host_compiler(self.tool,self.root/"first",env,capture):
            objects=[self.send(env,*v) for v in variants]
        self.assertEqual(objects[0],objects[1])
        self.assertEqual(len(set(objects)),len(variants)-1)
        self.assertEqual(len(list((capture/"shader-inputs").iterdir())),1)
        dest=self.root/"other"
        report=prewarm(self.tool,dest,from_run=capture,jobs=3)
        self.assertEqual(report["translations"],len(set(objects)))
        calls=self.calls()
        with host_compiler(self.tool,dest,env):
            self.assertEqual([self.send(env,*v) for v in variants],objects)
        self.assertEqual(self.calls(),calls)
        self.assertEqual(prewarm(self.tool,dest)["disk_hits"],len(set(objects)))
        (next((capture/"shader-inputs").iterdir())).write_bytes(b"source B"*8)
        with self.assertRaisesRegex(ValueError,"digest mismatch"):
            prewarm(self.tool,dest,from_run=capture)

    def test_compute_trial_does_not_replace_a_graphics_or_normal_compute_packet(self):
        alt=self.root/"trial"/"frosty-shader";alt.parent.mkdir()
        alt.write_text(COMPILER.replace("tag=hashlib.sha256(", 'tag=hashlib.sha256(b"trial"+'));alt.chmod(0o755)
        env={};cache=self.root/"shared"
        with host_compiler(self.tool,cache,env):
            ps=self.send(env,0,b"");cs=self.send(env,1797,b"")
        before=self.calls();env["FROSTY_COMPUTE_TOOL"]=str(alt)
        with host_compiler(self.tool,cache,env):
            self.assertEqual(self.send(env,0,b""),ps)
            self.assertNotEqual(self.send(env,1797,b""),cs)
        self.assertEqual(self.calls(),before)
        self.assertEqual((alt.parent/"invocations").read_text().splitlines(),["specialize 5 - 7"])

    def test_concurrent_warmers_and_corrupt_packet(self):
        root=self.root/"cache";a=ShaderCache(self.tool,root);b=ShaderCache(self.tool,root)
        req=Request.make(self.raw,773,b"\1")
        with ThreadPoolExecutor(4) as pool:
            results=list(pool.map(lambda store:store.resolve(req),[a,b,a,b]))
        self.assertEqual(len(set(results)),1);self.assertEqual(len(self.calls()),1)
        path=a.initial.directory/req.name
        data=bytearray(path.read_bytes());data[-1]^=1;path.write_bytes(data)
        self.assertEqual(b.resolve(req),results[0])
        self.assertEqual(len(self.calls()),2)
        self.assertEqual(b.summary()["invalid_packets"],1)

    def test_published_variant_is_not_queued_behind_a_cold_sibling(self):
        root=self.root/"cache";env={}
        ShaderCache(self.tool,root).resolve(Request.make(self.raw,773))
        entered,release=Event(),Event()
        resolve=ShaderCache.resolve
        def paused(store,request,*a,**kw):
            if request.params:
                entered.set()
                if not release.wait(6):raise RuntimeError("cached request waited for a compiler")
            return resolve(store,request,*a,**kw)
        with patch.object(ShaderCache,"resolve",paused),host_compiler(self.tool,root,env),ThreadPoolExecutor(2) as pool:
            cold=pool.submit(self.send,env,773,b"\1")
            try:
                self.assertTrue(entered.wait(3))
                warm=pool.submit(self.send,env,773,b"").result(timeout=2)
            finally:release.set()
            self.assertNotEqual(warm,cold.result())

    def test_unseen_material_uses_its_own_program_and_matching_typed_inputs(self):
        def original(body,output):
            chunks=[b'ISG1'+struct.pack('<I',1)+b'i',b'OSG1'+struct.pack('<I',1)+output,b'DXIL'+struct.pack('<I',1)+body]
            offsets=[];at=32+4*len(chunks)
            for chunk in chunks:offsets.append(at);at+=len(chunk)
            return b'DXBC'+bytes(16)+struct.pack('<III3I',1,at,3,*offsets)+b''.join(chunks)
        seed,candidate,other=(original(b'a',b'o'),original(b'b',b'o'),original(b'c',b'p'))
        root=self.root/"cache";store=ShaderCache(self.tool,root)
        r=Request.make(seed,0,b'\1');seed_code=store.resolve(r);store.remember(r)
        with patch("prewarm_shaders.archive_requests",return_value=[Request.make(b,0) for b in (candidate,other)]):
            report=prewarm_materials(self.tool,root,self.root/"fixture.pak")
        self.assertEqual((report["candidate_sources"],report["typed_variants"],report["errors"]),(1,1,[]))
        self.assertNotEqual(store.lookup(Request.make(candidate,0,b'\1'),store.initial),seed_code)
        self.assertIsNotNone(store.lookup(Request.make(candidate,0,b'\1'),store.initial))
        self.assertIsNone(store.lookup(Request.make(other,0,b'\1'),store.initial))

    def test_candidate_pipeline_preserves_root_vertex_state_and_pixel_identity(self):
        def shader(tag,psv=b"p"):
            chunks=[k+struct.pack("<I",len(v))+v for k,v in ((b"ISG1",b"i"),(b"OSG1",b"o"),(b"PSV0",psv),(b"DXIL",tag))]
            ats=[];end=48
            for c in chunks:ats.append(end);end+=len(c)
            return b"DXBC"+bytes(16)+struct.pack("<III4I",1,end,4,*ats)+b"".join(chunks)
        def blob(v):return struct.pack("<I",len(v))+v
        vs=shader(b"VS");root=self.root/"cache";store=ShaderCache(self.tool,root)
        store.resolve(Request.make(vs,1));d=root/"pipeline-recipes-v1";d.mkdir()
        pre=struct.pack("<II",0x31505358,2)+blob(b"original root")+bytes(32)+struct.pack("<II",1,28)+bytes(4+8+8*37+44+50)
        pre+=struct.pack("<I",1)+blob(b"POSITION\0")+struct.pack("<6I",0,6,0,0,0,0)+blob(vs)
        gs=blob(b"");h=lambda v:hashlib.sha256(v).hexdigest();hint=b"\0\1"
        for i in range(3):
            ps=shader(bytes([i]));store.resolve(Request.make(ps,0));v=pre+blob(ps)+gs
            (d/(h(v)+".plan")).write_bytes(v);(d/(h(v)+"."+h(hint)+".hint")).write_bytes(hint)
        new=shader(b"d");req=Request.make(new,0);store.resolve(req);store.resolve(Request.make(new,0,b"\1"))
        other=Request.make(shader(b"e",b"other binding table"),0);store.resolve(other)
        report=prepare_pipelines(store,[req,other]);self.assertEqual(report,{"templates":1,"new_plans":1,"new_hints":1})
        expected=pre+blob(new)+gs;fn=d/"predicted"/(h(expected)+".plan")
        self.assertEqual(fn.read_bytes(),expected);self.assertEqual(split_graphics(expected),(pre,vs,new,gs))
        self.assertEqual((fn.parent/(h(expected)+"."+h(hint)+".hint")).read_bytes(),hint)

    def test_build_and_dependency_changes_and_retry(self):
        root=self.root/"cache";req=Request.make(self.raw,5)
        store=ShaderCache(self.tool,root)
        (self.root/"fail-once").touch()
        with self.assertRaises(CompileError):store.resolve(req)
        store.resolve(req)
        self.assertEqual(len(self.calls()),2)
        ShaderCache(self.tool,root).resolve(req)
        self.assertEqual(len(self.calls()),2)
        self.tool.write_text(COMPILER+"\n# different translator\n")
        ShaderCache(self.tool,root).resolve(req)
        self.library="1"*64
        ShaderCache(self.tool,root).resolve(req)
        self.assertEqual(len(self.calls()),4)
        self.assertEqual(len(list((root/"packets-v1").glob("*/toolchain.json"))),3)

    def test_named_xsrt_retention_preserves_captures_without_inventing_a_recipe(self):
        store=ShaderCache(self.tool,self.root/"cache")
        raw=b"\2\0\0\0"+b"captured collection"*3
        key=0xcbf29ce484222325
        for byte in raw:key=(key*0x100000001b3&0xffffffffffffffff)^byte
        paths=[]
        for d in ("a","b"):
            run=self.root/d;run.mkdir();p=run/f"{key:016x}.xsrt";p.write_bytes(raw);paths.append(p)
            (run/"ffffffffffffffff.xsrt").write_bytes(raw[:-1])
            result=store.retain_xsrt(run,compact=True)
            self.assertEqual((result["originals"],result["invalid"],result["shared_files"]),(1,1,1))
        self.assertTrue(paths[0].samefile(paths[1]))
        self.assertEqual(len(list(store.inputs.iterdir())),1)
        self.assertFalse(list(store.recipes.iterdir()))


if __name__=="__main__":unittest.main()
