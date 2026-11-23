#!/usr/bin/env python3
"""Stage and run native AMD64 guest programs with the Frosty DLLs in Wine."""
import argparse
from contextlib import ExitStack, contextmanager
import fcntl
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
from shader_service import host_compiler
import hashlib
from prewarm_shaders import capture_manifest, prewarm, prewarm_materials

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("FROSTY_BUILD_ROOT", ROOT / "build")).expanduser().resolve()
BIN = Path("Frosty/Binaries/XSX")
TITLE = "Frosty-XSX-Shipping.exe"
SCHEDULING_CVARS={
    "r.RHICmdAsyncRHIThreadDispatch":(0,1),"r.RHICmdCollectRHIThreadStatsFromHighLevel":(0,1),
    "r.RHICmdFlushOnQueueParallelSubmit":(0,1),"r.RHICmdFlushRenderThreadTasks":(0,1),
    "r.RHICmdBalanceParallelLists":(0,1),"r.RHICmdWidth":(1,32),"r.OneFrameThreadLag":(0,1),
    "r.RDG.ParallelExecute.PassMin":(1,1024),"r.RDG.ParallelExecute.PassMax":(1,1024),
    "D3D12.CommandListBatchingMode":(0,2),"D3D12.MaximumFrameLatency":(1,3),"d3d12.BatchResourceBarriers":(0,1),
    "r.RayTracing.AsyncBuild":(0,1),"r.RayTracing.ParallelMeshBatchSetup":(0,1),"r.RayTracing.ParallelMeshBatchSize":(1,4096),
    "r.Nanite.AsyncRasterization":(0,1),"r.Nanite.Streaming.Async":(0,1),
    "r.DeferUniformExpressionCaching":(0,1),
    "r.Nanite.MaterialSortMode":(0,4),"r.Nanite.ParallelBasePassBuild":(0,1),"r.RHICmdMinDrawsPerParallelCmdList":(1,1024),
    "rhi.SyncInterval":(1,2),"rhi.SyncAllowEarlyKick":(0,1),
    "D3D12.AsyncDeferredDeletion":(0,1),"d3d12.FastAllocator.MinPagesToRetain":(1,64),
    "d3d12.UploadHeap.SmallBlock.PoolSize":(1<<20,64<<20),"d3d12.UploadHeap.BigBlock.PoolSize":(4<<20,128<<20),
    "r.GPUScene.MaxPooledUploadBufferSize":(1<<16,16<<20),
    "wp.Runtime.LevelStreamingContinuouslyIncrementalGCWhileLevelsPendingPurgeForWP":(0,2048),
    "gc.TimeBetweenPurgingPendingKillObjects":(1,60),
    "wp.Runtime.MaxLoadingStreamingCells":(1,64),"r.LumenScene.PrimitivesPerTask":(1,512),
    "r.LumenScene.MeshCardsPerTask":(1,512),
}

def scheduling_arg(text):
    key,sep,v=text.partition("=")
    if not sep or key not in SCHEDULING_CVARS or not v.isdecimal():raise argparse.ArgumentTypeError("expected a shipping scheduling variable and integer value")
    a,b=SCHEDULING_CVARS[key]
    if not a<=int(v)<=b:raise argparse.ArgumentTypeError(f"{key} needs {a}–{b}")
    return text


def required(path):
    path = Path(path).expanduser().resolve(strict=True)
    if not path.is_file():
        raise ValueError(f"expected a file: {path}")
    return path


def compiler_tool():
    return required(os.environ.get("FROSTY_SHADER_TOOL",BUILD/"host/frosty-shader"))


def mount_dir(root):
    if not root:
        raise ValueError("supply --game or FROSTY_GAME_ROOT (package or Mount directory)")
    root = Path(root).expanduser().resolve(strict=True)
    for mount in (root / "Mount", root):
        if (mount / BIN / TITLE).is_file():
            return mount
    raise ValueError(f"cannot find {BIN / TITLE} under {root}")


def play_mount(root):
    if root:return mount_dir(root)
    matches=[p for p in (Path.home()/"Downloads").glob("the-matrix-awakens*/4366*/Mount")
             if (p/BIN/TITLE).is_file()]
    if len(matches)==1:return mount_dir(matches[0])
    raise ValueError("set FROSTY_GAME_ROOT or use Play --game with the installed package")


def radeon_play_adapter():
    try:
        ids=[(p/"device").read_text().strip().lower() for p in Path("/sys/class/drm").glob("card[0-9]*/device")
             if re.fullmatch(r"card\d+",p.parent.name) and (p/"vendor").is_file() and (p/"vendor").read_text().strip().lower()=="0x1002"]
    except OSError:return False
    return ids==["0x73a5"]


def adapter_schedule(env):
    if radeon_play_adapter():
        env.setdefault("RADV_PERFTEST","pswave32,gewave32")
        env.setdefault("FROSTY_NATIVE_GROUP_ORDINAL","1")
        env.setdefault("VKD3D_CONFIG","retain_psos,no_upload_hvv")


def play_environment(env):
    # Diagnostic flags in the guest test for presence, not truthiness.
    for k in list(env):
        if k.startswith(("FROSTY_CAPTURE_", "FROSTY_TRACE_", "FROSTY_STREET_", "FROSTY_HOLD_", "FROSTY_WINDOW_")) or k in (
                "FROSTY_FRAME_TIMES", "FROSTY_STORAGE_TRACE", "FROSTY_OBSERVE_BREAKS", "FROSTY_LIVE_INPUT", "FROSTY_PERSIST_CONTROLLER",
                "VKD3D_SHADER_DUMP_PATH", "VKD3D_PROFILE_PATH", "VKD3D_AUTO_CAPTURE_SHADER", "VKD3D_QUEUE_PROFILE_PATH"):
            del env[k]
    env["FROSTY_PRESENT"]="dxgi"
    env.setdefault("FROSTY_ASYNC_HTTP_HEADERS","1")
    adapter_schedule(env)
    env.setdefault("VKD3D_CONFIG","retain_psos")
    env.setdefault("FROSTY_SPIRV_STAGES","1")
    env.setdefault("WINEDEBUG","-all")


def shader_tool(command, mount, *args):
    tool = compiler_tool()
    pak = required(mount / "Frosty/Content/Paks/Frosty-XSX.pak")
    subprocess.run([tool, command, pak, *args], check=True)


def library(root, name):
    root = Path(root).expanduser()
    for path in (root / name, root / "libs" / name[:-4] / name, root / "x64" / name):
        if path.is_file():
            return required(path)
    raise ValueError(f"missing {name} in {root}; build vkd3d-proton or set --vkd3d")


def link_siblings(src, dst, skip):
    # Create real ancestors of the guest EXE to preserve its relative content
    # paths even when using an alternative EXE from outside the package.
    for entry in src.iterdir():
        if entry.name.casefold() not in skip:
            (dst / entry.name).symlink_to(entry)


@contextmanager
def guest_volumes(prefix, view, grt, wine, env, userdata, system=None, core=None):
    """Game OS addresses content on G:, rather than using the EXE directory."""
    prefix.mkdir(parents=True, exist_ok=True)
    with (prefix / ".frosty-run.lock").open("a") as lock, ExitStack() as stack:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError(f"another title is using this prefix: {prefix}") from None
        journal=prefix/".frosty-mounts.json"
        allowed={"dosdevices/g:","dosdevices/t:","drive_c/windows/system32/xgameruntime.dll",
                 "drive_c/windows/system32/gameconfighelper.dll","drive_c/windows/system32/gamecoreclient.dll",
                 "drive_c/windows/system32/frosty_grt_core.dll"}
        if journal.exists():
            # A killed Wine/launcher pair can leave symlinks behind. Recover
            # only links recorded under this exact prefix and still unchanged.
            for name,target in json.loads(journal.read_text()).items():
                if name not in allowed:
                    raise ValueError(f"unexpected volume entry in {journal}")
                link=prefix/name
                if link.is_symlink() and str(link.readlink())==target:
                    link.unlink()
            journal.unlink()
        if not (prefix / "system.reg").exists():
            subprocess.run([wine, "wineboot", "-i"], env=env, check=True)
        scratch = userdata / "Temp"
        scratch.mkdir(parents=True, exist_ok=True)
        links = [(prefix / "dosdevices/g:", view), (prefix / "dosdevices/t:", scratch)]
        if core:
            links.append((prefix/"drive_c/windows/system32/frosty_grt_core.dll",required(core)))
        if grt:
            links.append((prefix / "drive_c/windows/system32/xgameruntime.dll", required(grt)))
            if system and required(grt)==(system/"xgameruntime.dll").resolve():
                for name in ("gameconfighelper.dll","gamecoreclient.dll"):
                    links.append((prefix/"drive_c/windows/system32"/name,required(system/name)))
        owned={}
        stack.callback(journal.unlink,missing_ok=True)
        for link, target in links:
            if link.is_symlink() and link.resolve() == target:
                continue
            if link.is_symlink() or link.exists():
                raise ValueError(f"existing {link} points elsewhere; use a dedicated --prefix")
            owned[str(link.relative_to(prefix))]=str(target)
            tmp=journal.with_suffix(".tmp");tmp.write_text(json.dumps(owned));tmp.replace(journal)
            link.symlink_to(target)
            stack.callback(link.unlink, missing_ok=True)
        yield


def run(args, guest_args):
    play=args.command=="play"
    title=play or args.command=="run"
    if platform.machine().lower() not in ("x86_64", "amd64"):
        raise ValueError("the native PE backend requires x86-64")
    wine = shutil.which(os.path.expanduser(args.wine))
    if not wine:
        raise ValueError(f"Wine loader not found: {args.wine}")
    # Resolve relative paths before changing cwd. Preserve the Wine executable
    # basename (some wine installations use a multicall/symlink entry point).
    wine = os.path.abspath(wine)
    libs = {n: library(args.vkd3d, n) for n in ("d3d12.dll", "d3d12core.dll")}
    dxgi = Path(args.dxgi).expanduser()
    if not dxgi.is_file():
        raise ValueError("set --dxgi or FROSTY_DXGI to a 64-bit DXVK dxgi.dll")
    libs["dxgi.dll"] = required(dxgi)
    libs["d3d12_xs.dll"] = required(os.environ.get("FROSTY_D3D12_XS",BUILD / "guest/d3d12_xs.dll"))
    mount = play_mount(args.game) if play else mount_dir(args.game) if title or args.command=="probe" else None
    dxc=args.dxc
    if not dxc and (mount or args.game):
        dxc=(mount or mount_dir(args.game)).parent / "EmbeddedXvd/Windows/System32/dxcompiler_xs.dll"
    if dxc:
        libs["dxcompiler_xs.dll"] = required(dxc)
    if mount:
        system = mount.parent / "EmbeddedXvd/Windows/System32"
        libs["xg_xs.dll"] = required(system/"xg_xs.dll")
        for name in ("AcpHal.dll","avcodec-62.dll","avutil-60.dll"):
            libs[name] = required(BUILD/"guest"/name)
        if args.package_input:
            libs["GameInput.dll"]=required(system/"GameInput.dll")
        else:
            libs["GameInput.dll"]=required(BUILD/"guest/frosty-gameinput.dll")
        libs["xmem.dll"] = required(BUILD / "guest/xmem.dll")
        libs["xcrdapi.dll"] = required(os.environ.get("FROSTY_XCRDAPI",BUILD/"guest/xcrdapi.dll"))
    local_fork = Path(wine) == BUILD / "wine64/wine"
    prefix = Path(args.prefix or BUILD / ("codec-test-prefix" if play else "fork-prefix" if local_fork else "wine-prefix")).expanduser().resolve()
    if mount and (prefix == mount or mount in prefix.parents):
        raise ValueError("choose a Wine prefix outside the package")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    runs = BUILD / "runs"
    runs.mkdir(parents=True, exist_ok=True)
    userdata = Path(args.userdata).expanduser().resolve()
    if mount and (userdata==mount or mount in userdata.parents):
        raise ValueError("choose a user data directory outside the package")
    shader_cache=Path(args.cache_dir).expanduser().resolve()

    with tempfile.TemporaryDirectory(dir=runs, prefix=args.command + "-") as tmp:
        view = Path(tmp) / "Mount"
        bin_dir = view / BIN
        bin_dir.mkdir(parents=True)
        files = dict(libs)
        test_cache = None
        if mount:
            files[TITLE] = required(args.exe or mount / BIN / TITLE)
        if args.command == "probe":
            program = "frosty-imports.exe"
            files[program] = required(BUILD / "guest" / program)
            argv = [TITLE]
        elif args.command=="media":
            program="frosty-media.exe"
            files[program]=required(BUILD/"guest"/program)
            files["xmem.dll"]=required(BUILD/"guest/xmem.dll")
            dll=required(mount_dir(args.game).parent/"EmbeddedXvd/Windows/System32/mfhevcdec.dll")
            argv=["Z:"+str(dll).replace("/","\\")]
            if args.register:
                if not args.prefix:raise ValueError("select the decoder's Wine prefix with --prefix")
                argv.append("--register")
                if args.surface_bridge:
                    bridge=required(BUILD/"guest/frosty-video.dll")
                    argv.append("Z:"+str(bridge).replace("/","\\"))
            elif args.surface_bridge:raise ValueError("--surface-bridge is a media registration option")
        elif args.command in ("ray-rows-check","zero-check"):
            program=f"frosty-{args.command}.exe";files[program]=required(BUILD/"guest"/program);argv=[]
        elif args.command=="storage-check":
            import struct,zlib
            program="frosty-storage-check.exe"
            files[program]=required(BUILD/"guest"/program);files["xcrdapi.dll"]=required(os.environ.get("FROSTY_XCRDAPI",BUILD/"guest/xcrdapi.dll"))
            blocks=[zlib.compress(bytes((i*13+offset)&255 for i in range(65536))) for offset in (7,53)]
            binary=struct.pack("<II",*(len(b) for b in blocks))+b"".join(blocks)
            (bin_dir/"storage-samples.bin").write_bytes(binary)
            argv=[]
            if args.pack_case:
                from storage_pack import publish,ROW,HEADER
                path=bin_dir/"fixture.pack";publish(path,[(8,len(blocks[0]),65536),(8+len(blocks[0]),len(blocks[1]),65536)],lambda o,n:binary[o:o+n])
                b=bytearray(path.read_bytes())
                if args.pack_case=="mixed":b[-9]^=1
                else:
                    for i in range(2):b[HEADER.size+i*ROW.size+24]^=1
                    b[16:48]=hashlib.sha256(b[HEADER.size:HEADER.size+2*ROW.size]).digest()
                path.write_bytes(b);argv=[args.pack_case]
        elif args.command in ("pipeline-check","pipeline-inspect"):
            checker=required(BUILD/"guest/frosty-pipeline-check.exe")
            # RADV/vkd3d select application workarounds, pipeline hashes and
            # the stream archive by application name. Give the standalone
            # baker the title's driver identity in its temporary directory.
            program=TITLE if args.command=="pipeline-inspect" and not args.separate_app_cache else checker.name
            files[program]=checker
            argv=["--prepare"] if args.command=="pipeline-inspect" else []
        elif args.command in ("upload-check","tile-prewarm"):
            program="frosty-upload-check.exe"
            files[program]=required(BUILD/"guest"/program)
            files["xg_xs.dll"]=required(play_mount(args.game).parent/"EmbeddedXvd/Windows/System32/xg_xs.dll")
            files["xmem.dll"]=required(BUILD/"guest/xmem.dll")
            argv=["--prepare-layouts"] if args.command=="tile-prewarm" else []
        elif args.command in ("isa-check","hwrt-check","kernel-pair","vertex-pair","material-depth-check"):
            program=f"frosty-{args.command}.exe"
            files[program]=required(BUILD/"guest"/program)
            suite=Path(args.suite).resolve(strict=True)
            if not suite.is_dir():raise ValueError("expected GPU fixture directory")
            if args.command=="vertex-pair" and (suite/"reference.rect.dxil").is_file():
                subprocess.run([required(BUILD/"deps/dxc/bin/dxc"),"-T","ps_6_0","-E","main","-Fo",bin_dir/"tile-ps.dxil",ROOT/"guest/test_rect_outputs.hlsl"],check=True)
            argv=["Z:"+str(suite).replace("/","\\")]
        elif args.command in ("draw", "clear", "scene-ids", "max-pixel"):
            program = f"frosty-{args.command}.exe"
            files[program] = required(BUILD / "guest" / program)
            if args.shader:
                shader = required(args.shader)
            else:
                output = Path(tmp) / "copy-vs"
                index = str(args.index) if args.command=="clear" else {"draw":"0","scene-ids":"1320","max-pixel":"2633"}[args.command]
                shader_tool("dump", mount_dir(args.game), "Global", index, output)
                (shader,) = output.glob("*.dxbc")
            files["input.dxbc"] = shader
            argv = ["input.dxbc"] + (["--test-mf"] if args.command=="draw" and args.test_mf else [])
            if args.command=="draw" and args.test_rectlist:argv.append("--rectlist")
            if args.command=="draw" and (args.test_pixel_wave or args.test_pixel_cube or args.test_pixel_interp):
                mode="cube" if args.test_pixel_cube else "interp" if args.test_pixel_interp else "wave"
                obj=Path(tmp)/"fixture.o";isa=Path(tmp)/"fixture.bin";dx=Path(tmp)/"fixture.dxil"
                subprocess.run(["llvm-mc","-triple=amdgcn-amd-amdhsa","-mcpu=gfx1030","-mattr=+wavefrontsize64,-wavefrontsize32","-filetype=obj",
                                ROOT/f"guest/test_{mode}_pixel.s","-o",obj],check=True)
                subprocess.run(["llvm-objcopy","--only-section=.text","-O","binary",obj,isa],check=True)
                subprocess.run([BUILD/"host/frosty-shader",f"test-pixel-{mode}",isa,dx],check=True)
                files[dx.name]=dx;argv.extend([f"--{mode}-pixel",dx.name])
            if args.command=="draw" and args.test_zero_coverage:
                from cooked_pairs import signature
                sig=signature(required(args.test_zero_coverage).read_bytes(),b"ISG1")
                if sig is None:raise ValueError("missing coverage pixel shader input signature")
                uv=any(s[0]==b"TEXCOORD" and s[1]==0 for s in sig)
                vs=Path(tmp)/"coverage-vs.dxil"
                subprocess.run([required(BUILD/"deps/dxc/bin/dxc"),"-T","vs_6_0","-E","main","-D",f"COVERAGE_UV={int(uv)}","-Fo",vs,ROOT/"guest/test_coverage.hlsl"],check=True)
                files[vs.name]=vs
                files["coverage.dxil"]=required(args.test_zero_coverage);argv+=["--zero-coverage","coverage.dxil"]
            if args.command=="clear":
                argv.append(str(args.group_width))
                if args.index==339:
                    argv.append("--indirect")
            if args.command=="draw" and (args.test_pixel or args.test_geometry):
                if not args.game or args.shader:
                    raise ValueError("stage readback requires --game and the default draw shader")
                stage = "pixel" if args.test_pixel else "geometry"
                test_cache = Path(tmp) / stage
                shader_tool("dump", mount_dir(args.game), "Global", "2536" if args.test_pixel else "2420", test_cache)
                (stage_file,) = test_cache.glob("*.dxbc")
                filename = stage + ".dxbc"
                files[filename] = stage_file
                argv.extend(["--" + stage, filename])
            if dxc and args.command=="draw":
                argv.append("--test-dxc")
        else:
            program = TITLE
            argv = ["-NoLoadingScreen"] if play else []
            if play and args.mute:argv.append("-nosound")
            argv.extend(guest_args)
        if mount:
            src, dst = mount, view
            for part in BIN.parts:
                skip = {part.casefold()}
                if src == mount / "Frosty":
                    saved = userdata / "Saved"
                    saved.mkdir(parents=True, exist_ok=True)
                    (dst / "Saved").symlink_to(saved)
                    skip.add("saved")
                    if title:
                        config = dst / "Config"
                        config.mkdir()
                        chase_gc=args.chase_purge_levels if play else None
                        if (src / "Config").is_dir():
                            link_siblings(src/"Config",config,{"userengine.ini"}|({"userdeviceprofiles.ini"} if chase_gc is not None else set()))
                        ini=src/"Config/UserEngine.ini"
                        text=ini.read_text() if ini.is_file() else ""
                        profile=required(ROOT/"profiles"/args.profile/"UserEngine.ini")
                        text+="\n"+profile.read_text()
                        required_on={"r.nanite","r.dynamicglobalilluminationmethod","r.reflectionmethod",
                                     "r.lumen.diffuseindirect.allow","r.lumen.reflections.allow"}
                        options=list(args.cvar or ())
                        if play:options.extend(args.schedule)
                        if play and args.async_compute:options+=["r.RDG.AsyncCompute=2","r.Bloom.AsyncCompute=1","r.EnableAsyncComputeTranslucencyLightingVolumeClear=1"]
                        if options:text+="\n[SystemSettings]\n"
                        for kv in options:
                            if not re.fullmatch(r"[\w.]+=[^\r\n]+",kv):raise ValueError(f"invalid console setting: {kv!r}")
                            key,val=kv.split("=",1)
                            if key.casefold() in required_on and val.strip()!="1":
                                raise ValueError(f"{key} must stay enabled in the gameplay profile")
                            text+="\n"+kv
                        if play:
                            for kv in args.schedule:
                                if kv.startswith("gc.TimeBetweenPurgingPendingKillObjects="):
                                    text+="\n[/Script/Engine.GarbageCollectionSettings]\n"+kv+"\n"
                        (config/"UserEngine.ini").write_text(text+"\n")
                        if chase_gc is not None:
                            # ApplyChaseLoadingSettings sets the backing level
                            # threshold to 32. The title then pushes its own
                            # cine profiles; use that existing scheduling hook.
                            base=src/"Config/UserDeviceProfiles.ini"
                            layers=base.read_text() if base.is_file() else ""
                            for section in ("XSX_ChaseCine","XSX_BloomFixCine","XSX_ChaseGameplay"):
                                layers+=f"\n[{section} DeviceProfile]\n+CVars=s.ContinuouslyIncrementalGCWhileLevelsPendingPurge={chase_gc}\n"
                            (config/"UserDeviceProfiles.ini").write_text(layers)
                        skip.add("config")
                link_siblings(src, dst, skip)
                src, dst = src / part, dst / part
            link_siblings(src, dst, {name.casefold() for name in files})
        for name, path in files.items():
            (bin_dir / name).symlink_to(path)

        env = os.environ.copy()
        if args.command=="storage-check" and args.pack_case:env["FROSTY_STORAGE_PACK"]="Z:"+str(bin_dir/"fixture.pack").replace("/","\\")
        env["WINEPREFIX"] = str(prefix)
        rt_dir=Path(args.suite) if args.command=="hwrt-check" else None
        if play:play_environment(env)
        if args.command=="pipeline-inspect":
            if not args.separate_app_cache:adapter_schedule(env)
            env.setdefault("FROSTY_SPIRV_STAGES","1");env.setdefault("VKD3D_CONFIG","retain_psos")
        if args.command=="tile-prewarm":
            layouts=shader_cache/"texture-layouts-v1"/hashlib.sha256(files["xg_xs.dll"].read_bytes()).hexdigest()
            layouts.mkdir(parents=True,exist_ok=True);env["FROSTY_TILE_PLANS"]="Z:"+str(layouts).replace("/","\\")
        if env.get("FROSTY_SPIRV_STAGES")=="1" and "FROSTY_SPIRV_DIR" not in env:
            # Device layout, exact DXIL, stage parameters and root bindings
            # are validated by vkd3d. Segregate local converter binaries too,
            # even when multiple dirty builds report the same Git revision.
            binary=hashlib.sha256(libs["d3d12core.dll"].read_bytes())
            for name in ("DXIL_SPIRV_CONFIG","VKD3D_CONFIG","VKD3D_SHADER_QUIRKS"):
                binary.update(b"\0"+name.encode()+b"="+env.get(name,"").encode())
            stage_dir=shader_cache/"spirv-stages-v1"/binary.hexdigest();stage_dir.mkdir(parents=True,exist_ok=True)
            env["FROSTY_SPIRV_DIR"]="Z:"+str(stage_dir).replace("/","\\")
        if args.command=="pipeline-inspect":
            plans=Path(args.suite).resolve(strict=True)
            if not plans.is_dir():raise ValueError("expected a directory of recorded pipeline recipes")
            env["FROSTY_PIPELINE_RECIPES"]="Z:"+str(plans).replace("/","\\")
        if title:
            if args.rt_libraries:rt_dir=Path(args.rt_libraries)
            elif (BUILD/"rt-libraries").is_dir():rt_dir=BUILD/"rt-libraries"
        if rt_dir is not None:
            if not rt_dir.is_dir():raise ValueError(f"expected translated ray-library directory: {rt_dir}")
            env["FROSTY_RT_LIBRARY_PATH"]="Z:"+str(rt_dir.resolve()).replace("/","\\")
        if title and args.rt_dispatch_libraries:
            dispatch=Path(args.rt_dispatch_libraries).expanduser().resolve(strict=True)
            if not dispatch.is_dir():raise ValueError(f"expected ray dispatch library directory: {dispatch}")
            env["FROSTY_RT_DEMAND_PATH"]="Z:"+str(dispatch).replace("/","\\")
        if title:
            env["FROSTY_PRESENT"]="dxgi"
            if play:
                store=BUILD/"play-runtime/storage/blocks.pack"
                if store.is_file():env.setdefault("FROSTY_STORAGE_PACK","Z:"+str(store).replace("/","\\"))
                plans=shader_cache/"pipeline-recipes-v1";plans.mkdir(parents=True,exist_ok=True)
                env["FROSTY_PIPELINE_RECIPES"]="Z:"+str(plans).replace("/","\\")
                layouts=shader_cache/"texture-layouts-v1"/hashlib.sha256(libs["xg_xs.dll"].read_bytes()).hexdigest()
                layouts.mkdir(parents=True,exist_ok=True);env["FROSTY_TILE_PLANS"]="Z:"+str(layouts).replace("/","\\")
            if args.frame_times:
                target=Path(args.frame_times).expanduser().resolve()
                target.parent.mkdir(parents=True,exist_ok=True)
                env["FROSTY_FRAME_TIMES"]="Z:"+str(target).replace("/","\\")
            if play and args.storage_reads:
                target=Path(args.storage_reads).resolve();target.parent.mkdir(parents=True,exist_ok=True)
                env["FROSTY_STORAGE_TRACE"]="Z:"+str(target).replace("/","\\")
        if args.command == "run" and args.capture_dir:
            capture = Path(args.capture_dir).expanduser().resolve()
            capture.mkdir(parents=True, exist_ok=True)
            env["FROSTY_CAPTURE_DIR"] = "Z:" + str(capture).replace("/", "\\")
        env["WINEDLLOVERRIDES"] = ";".join(filter(None, [
            env.get("WINEDLLOVERRIDES"), "d3d12,d3d12core,d3d12_xs,dxgi=n",
            "gameinput=n" if "GameInput.dll" in libs else "",
            "winemenubuilder.exe="]))
        override = args.overrides or test_cache
        if args.command in ("draw","clear","scene-ids","max-pixel") and not override:
            override = shader.parent
        if override:
            folder = Path(override).expanduser().resolve(strict=True)
            if not folder.is_dir():
                raise ValueError(f"expected a shader override directory: {folder}")
            # The guest reads desktop DXIL before handing the PSO to vkd3d.
            env["FROSTY_SHADER_PATH"] = "Z:" + str(folder).replace("/", "\\")
        # Keep vkd3d caches out of the temporary title view.
        cache = BUILD / "shader-cache"
        cache.mkdir(exist_ok=True)
        env.setdefault("VKD3D_SHADER_CACHE_PATH", "Z:" + str(cache).replace("/", "\\"))
        print(f"{args.command}: {files[program]}\nWine: {wine} | prefix: {prefix}", flush=True)
        if title:print(f"Renderer profile: {args.profile} | GPU-only DXGI | title arguments: {argv!r}",flush=True)
        with ExitStack() as stack:
            if args.command not in ("probe","isa-check","kernel-pair","vertex-pair","material-depth-check","upload-check","tile-prewarm","pipeline-check","storage-check","ray-rows-check","zero-check","hwrt-check","media") and "-nullrhi" not in [a.lower() for a in argv]:
                tool=compiler_tool()
                trace_dir=capture if args.command=="run" and args.capture_dir else None
                if args.command=="run" and args.prepare_shaders:
                    # A failed optional archive permutation is reported. It
                    # must not stop an unrelated level or become a live hit.
                    prewarm(tool,shader_cache,pak=mount/"Frosty/Content/Paks/Frosty-XSX.pak",archive=True,
                            report=trace_dir/"shader-prewarm.json" if trace_dir else None)
                stack.enter_context(host_compiler(tool,shader_cache,env,trace_dir))
            if title:
                grt=system/"xgameruntime.dll" if args.grt=="package" else args.grt
                core=None
                if grt and args.grt!="package" and Path(grt).resolve()==(BUILD/"grt/bin/xgameruntime.dll").resolve():
                    userdata.mkdir(parents=True,exist_ok=True)
                    local=userdata/"local-player.id"
                    try:
                        with local.open("x") as f:
                            f.write(f"{0xe000000000000000 | (uuid.uuid4().int & 0x0000ffffffffffff):016x}\n")
                    except FileExistsError:
                        pass
                    env["FROSTY_LOCAL_ID"]=f"{int(local.read_text().strip(),16):016x}"
                    core=required(grt)
                    grt=required(BUILD/"guest/frosty-local-grt.dll")
                stack.enter_context(guest_volumes(prefix, view, grt, wine, env, userdata, system,core))
            cmd = [wine, str(bin_dir / program), *argv]
            if args.command == "run" and args.debug_script:
                script = "Z:" + str(required(args.debug_script)).replace("/", "\\")
                cmd = [wine, "winedbg", "--file", script, "G:\\" + str(BIN / TITLE).replace("/", "\\"), *argv]
            result = subprocess.run(cmd, cwd=bin_dir, env=env)
        return result.returncode if result.returncode >= 0 else 128 - result.returncode


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    cache_dir=os.environ.get("FROSTY_SHADER_CACHE",BUILD/"specializations")
    warm=sub.add_parser("prewarm",help="precompile exact recorded shader variants into the live cache without launching Wine")
    mode=warm.add_mutually_exclusive_group()
    mode.add_argument("--from-run",type=Path,help="capture directory, shader-requests.jsonl or legacy launch.log")
    mode.add_argument("--archive",action="store_true",help="also warm default VS/PS/GS/CS stages from both cooked libraries")
    mode.add_argument("--material-families",action="store_true",help="also compile sibling pixel shaders sharing the recorded input/output signatures, including compatible typed views")
    mode.add_argument("--stage-families",action="store_true",help="prepare permutations of recorded global shader types, vertex interfaces and compatible compute pipeline recipes")
    mode.add_argument("--cooked-pairs",action="store_true",help="prepare plausible VS/PS pairs from the original cooked material maps")
    mode.add_argument("--all-graphics",action="store_true",help="prepare all supported original graphics stages, with compatible recorded view layouts")
    warm.add_argument("--pair-limit",type=int,default=600,help="maximum representative native recipes for --cooked-pairs")
    warm.add_argument("--game",default=os.environ.get("FROSTY_GAME_ROOT"),help="required for --archive or a legacy trace")
    warm.add_argument("--wave-caps","--compute-caps",choices=["all",*map(str,range(16))],default="all",help="compute capabilities: bits 0/1 = wave32/64, bit 2 = native FP16, bit 3 = backend subgroup ordinal")
    warm.add_argument("--jobs",type=int,default=min(6,os.cpu_count() or 1))
    warm.add_argument("--report",type=Path,help="write counts and failures as JSON")
    mode.add_argument("--raster-siblings",action="store_true",help="prepare native recipes for observed cull modes and bound views of compatible cooked pairs")
    mode.add_argument("--all-compute",action="store_true",help="prepare every cooked compute stage and compatible recorded view types for one device mask")
    warm.add_argument("--cache-dir",default=cache_dir)
    steam_dxgi = (Path.home() / ".local/share/Steam/steamapps/common/Proton - Experimental"
                  / "files/lib/wine/dxvk/x86_64-windows/dxgi.dll")
    built_dxgi = BUILD / "dxvk/src/dxgi/dxgi.dll"
    for command, help_text in (("probe", "resolve title imports in the guest loader"),
                               ("run", "launch the title; pass guest arguments after --"),
                               ("play","play the shipping game on the GPU swapchain, with no diagnostic capture"),
                               ("draw", "verify a lifted CopyVS with offscreen D3D12 readback"),
                                ("scene-ids","verify the retail GPUScene shared-memory compute shader"),
                                ("max-pixel","verify tiled luminance reduction and cross-group atomics on the GPU"),
                                  ("isa-check","execute assembled RDNA 2 conformance cases and compare GPU readbacks"),
                                   ("kernel-pair","compare the reference and candidate cooked image-convolution kernels on GPU"),
                                    ("vertex-pair","compare original material-tile vertex shader translations via GPU stream output"),
                                    ("material-depth-check","verify the shipping pixel shader's early depth tests and UAV writes on GPU"),
                                   ("ray-rows-check","verify lossless TLAS metadata compaction and native command-list state on two queues"),
                                   ("zero-check","verify cold and recycled default memory contents across two queues"),
                                 ("upload-check","check fenced texture reuploads between two GPU queues"),
                                 ("tile-prewarm","prepare exact GPU detiling maps from learned Xbox texture layouts without starting the title"),
                                  ("pipeline-check","verify replayed native pipelines against GPU output on the active adapter"),
                                  ("pipeline-inspect","create a set of recorded native pipelines without starting the game or dispatching work"),
                                  ("storage-check","verify ordered asynchronous packaged reads, status markers and fence completion"),
                                 ("hwrt-check","build BLAS/TLAS through XS and verify hardware ray results on packaged geometry"),
                                ("media","check the packaged HEVC decoder using Electra's enumeration filter"),
                                ("clear","verify the retail ClearCS and typed descriptor bindings with GPU readback")):
        c = sub.add_parser(command, help=help_text)
        c.add_argument("--cache-dir",default=cache_dir,help="persistent translation cache (FROSTY_SHADER_CACHE)")
        fork = BUILD / "wine64/wine"
        c.add_argument("--wine", default=os.environ.get("FROSTY_WINE", str(fork) if fork.is_file() else "wine"))
        c.add_argument("--prefix", default=os.environ.get("FROSTY_PREFIX"))
        c.add_argument("--userdata", default=os.environ.get("FROSTY_USERDATA",BUILD/("enter-verified-user" if command=="play" else "userdata")),
                       help="location of title logs, settings and saves")
        c.add_argument("--vkd3d", default=os.environ.get("FROSTY_VKD3D", BUILD / "vkd3d"))
        c.add_argument("--dxgi", default=os.environ.get("FROSTY_DXGI", built_dxgi if built_dxgi.is_file() else steam_dxgi))
        c.add_argument("--dxc", default=os.environ.get("FROSTY_DXC"),
                       help="override the package's dxcompiler_xs.dll for the DxcUtils import")
        c.add_argument("--overrides", help="directory with recompiled <shader-hash>.<stage>*.dxil files")
        c.add_argument("--game", default=os.environ.get("FROSTY_GAME_ROOT"))
        if command=="media":
            c.add_argument("--register", action="store_true", help="register the original package decoder in the selected Wine prefix")
            c.add_argument("--surface-bridge", action="store_true", help="upload the packaged decoder's software frames as XS D3D12 luma/chroma surfaces")
        elif command=="storage-check":
            c.add_argument("--pack-case",choices=("mixed","stale"),help="exercise valid, corrupt-output and changed-input cache entries")
        elif command in ("isa-check","hwrt-check","pipeline-inspect","kernel-pair","vertex-pair","material-depth-check"):
            c.add_argument("--suite",required=True,help="fixture directory generated by isa_conformance.py or hwrt_fixture.py")
            if command=="pipeline-inspect":
                c.add_argument("--separate-app-cache",action="store_true",help="measure the helper's own driver profile instead of the shipping title's")
        elif command in ("draw","clear","scene-ids","max-pixel"):
            c.add_argument("--shader", help="original shader .dxbc; default: lift from --game")
            if command=="draw":
                c.add_argument("--test-mf", action="store_true", help="exercise the Wine fork's media-device export")
                stage=c.add_mutually_exclusive_group()
                stage.add_argument("--test-rectlist",action="store_true",help="verify the fourth corner and its UVs from three retail CopyVS vertices")
                stage.add_argument("--test-pixel",action="store_true",help="read back Global[2536] pixel interpolation and full-width color export")
                stage.add_argument("--test-pixel-wave",action="store_true",help="check masked elections and relative VGPR addressing in a pixel shader")
                stage.add_argument("--test-zero-coverage",type=Path,metavar="DXIL",help="verify a lifted null-coverage shader against a cleared GPU render target")
                stage.add_argument("--test-pixel-cube",action="store_true",help="check sampled cube array faces, layers and biased mip levels")
                stage.add_argument("--test-pixel-interp",action="store_true",help="check biased mips after masked P1/P2 interpolation against GPU pixels")
                stage.add_argument("--test-geometry",action="store_true",help="verify Global[2420] triangle emission to two array slices")
            elif command=="clear":
                c.add_argument("--index",type=int,default=402,help="Global[] bounded uint4 clear (default 402), or indirect argument clear (339)")
                c.add_argument("--group-width",type=int,default=64,help="reference group width for the two-group readback")
        elif command!="upload-check":
            c.add_argument("--exe", help="alternative title executable (requires the base package)")
            c.add_argument("--package-input",action="store_true",help="use the packaged Xbox GameInput provider")
        if command in ("play","run"):
            c.add_argument("--rt-libraries",default=os.environ.get("FROSTY_RT_LIBRARIES"),help="source-matched SPTR libraries produced by frosty-shader rt-lift")
            active_rays=next((d for d in (BUILD/"play-runtime/rays",BUILD/"street-shadow-demand")
                              if (d/"dispatch.contract").is_file()), ROOT/"profiles/play/rays")
            c.add_argument("--rt-dispatch-libraries",default=os.environ.get("FROSTY_RT_DISPATCH_LIBRARIES",active_rays if command=="play" else None),help="source-matched, GPU-checked raygen layouts and per-dispatch shader table linking")
            c.add_argument("--frame-times",metavar="TSV",help="optionally time successful GPU presents (timestamps only)")
            grt = BUILD / "grt/bin/xgameruntime.dll"
            c.add_argument("--grt", default=os.environ.get("FROSTY_GRT", grt if grt.is_file() else None),
                           help="x64 Gaming Runtime provider for the system32-only lookup, or 'package'")
        if command=="play":
            c.add_argument("--mute",action="store_true",help="start with the game's audio disabled")
            c.add_argument("--storage-reads",type=Path,help=argparse.SUPPRESS)
            c.add_argument("--schedule",action="append",type=scheduling_arg,default=[],metavar="CVAR=INT",help="trial a shipping scheduling setting")
            c.add_argument("--chase-purge-levels",type=int,choices=range(0,2049),metavar="N",help="apply a level-purge threshold through the shipping chase device profiles")
            c.add_argument("--async-compute",action="store_true",help="schedule all compute-list eligible RDG passes on the asynchronous compute path")
            c.set_defaults(profile="play",cvar=(),capture_dir=None,prepare_shaders=False,debug_script=None)
        if command=="run":
            c.add_argument("--cvar",action="append",default=[],metavar="NAME=VALUE",help="stage a game-recognized SystemSettings entry for this run")
            c.add_argument("--present",choices=["dxgi"],default="dxgi",help="GPU-only swapchain presentation")
            c.add_argument("--capture-dir", help="developer shader and queue diagnostics (no scanout readback)")
            c.add_argument("--debug-script", help="run the title under the selected WineDbg command file")
            c.add_argument("--gameplay-profile","--menu-profile",dest="profile",action="store_const",const="gameplay",default="gameplay",
                           help="stage the default gameplay settings: Nanite, Lumen GI and Lumen reflections")
            c.add_argument("--prepare-shaders", action="store_true",
                             help="prewarm recorded variants and supported archive base stages into the live cache before launch")
    argv = sys.argv[1:]
    split = argv.index("--") if "--" in argv else len(argv)
    args = p.parse_args(argv[:split])
    if split < len(argv) and args.command not in ("run","play"):
        p.error("guest arguments only apply to play or run")
    try:
        if args.command=="prewarm":
            if args.raster_siblings:
                from raster_families import prepare
                prepare(compiler_tool(),args.cache_dir,args.report);return 0
            if args.all_graphics or args.all_compute:
                from prewarm_graphics import prepare
                if args.all_compute and (args.all_graphics or args.wave_caps=="all"):raise ValueError("--all-compute needs one --compute-caps mask")
                caps=int(args.wave_caps) if args.all_compute else None
                result=prepare(compiler_tool(),args.cache_dir,
                               play_mount(args.game)/"Frosty/Content/Paks/Frosty-XSX.pak",args.jobs,args.report,caps)
                return int(bool(result["errors"]))
            if args.cooked_pairs:
                from cooked_pairs import prepare
                if not 0<args.pair_limit<=8192 or not 1<=args.jobs<=32:raise ValueError("invalid cooked candidate limit or job count")
                pak=play_mount(args.game)/"Frosty/Content/Paks/Frosty-XSX.pak"
                result=prepare(compiler_tool(),required(BUILD/"host/frosty-material-index"),
                               args.cache_dir,pak,args.jobs,args.pair_limit,args.report)
                return int(bool(result["errors"]))
            if args.stage_families:
                from stage_families import prepare
                if args.wave_caps=="all":raise ValueError("select the target adapter's --compute-caps mask for stage-family prediction")
                mount=play_mount(args.game);pak=mount/"Frosty/Content/Paks/Frosty-XSX.pak"
                index=BUILD/"gameplay-index";tool=compiler_tool()
                if not (index/"types.tsv").is_file():
                    subprocess.run([tool,"global-index",pak,index],check=True)
                    subprocess.run([sys.executable,ROOT/"tools/retail_shader_types.py",index,mount/"Symbols/Frosty-XSX-Shipping-Symbols.bin"],check=True)
                result=prepare(tool,args.cache_dir,pak,index/"types.tsv",int(args.wave_caps),args.jobs,args.report)
                return int(bool(result["errors"]))
            if args.material_families:
                pak=play_mount(args.game)/"Frosty/Content/Paks/Frosty-XSX.pak"
                result=prewarm_materials(compiler_tool(),args.cache_dir,pak,args.jobs,args.report)
                return int(bool(result["errors"]))
            needs_pak=args.archive or (args.from_run is not None and capture_manifest(args.from_run) is None)
            pak=mount_dir(args.game)/"Frosty/Content/Paks/Frosty-XSX.pak" if needs_pak and args.game else None
            result=prewarm(compiler_tool(),args.cache_dir,pak=pak,archive=args.archive,
                           from_run=args.from_run,jobs=args.jobs,wave_caps=args.wave_caps,report=args.report)
            return int(bool(result["errors"]))
        return run(args, argv[split + 1:])
    except subprocess.CalledProcessError as e:
        return e.returncode
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError) as e:
        print(f"frosty: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
