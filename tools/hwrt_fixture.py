#!/usr/bin/env python3
"""Extract the packaged indexed cube and construct an independent ray-hit oracle."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import shutil
import subprocess
from retail_mesh_fields import IoStore, meshes, static_mesh_class

ROOT = Path(__file__).resolve().parents[1]
ASSET = "Engine/Content/BasicShapes/Cube.uasset"


def indexed_mesh(b):
    for match in re.finditer(rb"\x0c\x00\x00\x00", b):
        at = match.start()
        if at+16 > len(b):
            continue
        stride, n, elem, count = struct.unpack_from("<4I", b, at)
        end = at+16+stride*n
        if n < 3 or elem != 12 or count != n or end+26 > len(b):
            continue
        vertices = b[at+16:end]
        tex, nv, fp, hp, tangent, nt = struct.unpack_from("<6I", b, end+2)
        if nv != n or not 1 <= tex <= 8 or max(fp, hp) > 1 or tangent not in (8, 16) or nt != n:
            continue
        end += 26+tangent*nt
        if end+8 > len(b):
            continue
        uv, nu = struct.unpack_from("<2I", b, end)
        if uv not in (4, 8) or nu != n*tex:
            continue
        end += 8+uv*nu
        if end+10 > len(b):
            continue
        color, nc = struct.unpack_from("<2I", b, end+2)
        end += 10
        if nc:
            if nc != n or color != 4 or end+8 > len(b) or struct.unpack_from("<2I", b, end) != (4, n):
                continue
            end += 8+4*n
        if end+12 > len(b):
            continue
        wide, width, size = struct.unpack_from("<3I", b, end)
        if wide > 1 or width != 1 or not size or size % (6 << wide) or end+12+size > len(b):
            continue
        idx = [v for v, in struct.iter_unpack("<I" if wide else "<H", b[end+12:end+12+size])]
        if max(idx) < n:
            return vertices, idx
    raise ValueError("no validated position/index stream")


def fixture(utoc, out, lumen_miss=False, intersection=False, position_fetch=False):
    store = IoStore(utoc)
    chunk = next(i for path, i in store.files_in() if path == ASSET)
    (_, pos, size), = meshes(store, chunk, static_mesh_class(utoc.with_name("global.ucas")))
    vertices, indices = indexed_mesh(store.read(chunk, pos, size))
    points = list(struct.iter_unpack("<3f", vertices))
    # +Z rays, two instances of the original geometry. Evaluate intersections
    # with all indexed triangles in XY, independent of the Vulkan BVH.
    rays = [(13, 11, 1, 600), (213, 11, 2, 600), (213, 11, 1, 600), (13, 11, 1, 75),
            (88, 5, 255, 600), (-23, 8, 255, 600), (218, -21, 255, 600), (13, 11, 0, 600)]
    def intersect(x, y, mask, far):
        result = (0xffffffff, 0xffffffff, 0., 0., 0.)
        for dx, dz, bit, ID in ((0, 0, 1, 17), (200, 80, 2, 41)):
            if not mask & bit:
                continue
            for j in range(0, len(indices), 3):
                a, b, c = (points[indices[j+k]] for k in range(3))
                vx, vy = b[0]-a[0], b[1]-a[1]
                wx, wy = c[0]-a[0], c[1]-a[1]
                px, py = x-dx-a[0], y-a[1]
                det = vx*wy-vy*wx
                if abs(det) < 1e-8:
                    continue
                u, v = (px*wy-py*wx)/det, (vx*py-vy*px)/det
                t = a[2]+u*(b[2]-a[2])+v*(c[2]-a[2])+dz+160
                if min(u, v, 1-u-v) > 0 and 0 < t < far and (result[0] == 0xffffffff or t < result[2]):
                    result = (ID, j//3, t, u, v)
        return result
    records = bytearray()
    position_ref = bytearray()
    occlusion = bytearray()
    hits = 0
    for x, y, mask, far in rays:
        result = intersect(x, y, mask, far)
        hits += result[0] != 0xffffffff
        if position_fetch:
            verts = (points[indices[result[1]*3+j]] for j in range(3)) if result[0] != 0xffffffff else [(0, 0, 0)]*3
            for xyz in verts:position_ref.extend(struct.pack("<3f", *xyz))
        # FOcclusionMainRG uses the static instance mask 1.
        obj, prim, t, u, v = intersect(x, y, 1, far)
        if intersection:
            # Reverse the two TLAS rows for this check so InstanceIndex() of
            # the mask-1 instance is nonzero and distinct from its InstanceID.
            occlusion.extend(struct.pack("<fIIff", t, prim, 1, u, v) if obj != 0xffffffff
                             else struct.pack("<5f", -1, 0, 0, 0, 0))
        else:
            occlusion.extend(struct.pack("<I", 0xffffffff if obj != 0xffffffff else 0))
        records.extend(struct.pack("<fffI", x, y, far, mask))
        records.extend(struct.pack("<IIfff", *result))
    if hits != 4:
        raise ValueError("unexpected packaged mesh intersections")
    out.mkdir(parents=True, exist_ok=True)
    data = struct.pack("<3I", len(points), len(indices), len(rays))+vertices+struct.pack(f"<{len(indices)}I", *indices)+records
    (out/"cube-rays.bin").write_bytes(data)
    (out/"occlusion.ref").write_bytes(occlusion)
    if position_fetch:(out/"positions.ref").write_bytes(position_ref)
    else:(out/"positions.ref").unlink(missing_ok=True)
    extra = ["-D", "FROSTY_QUERY_POSITIONS=1"] if position_fetch else []
    subprocess.run([ROOT/"build/deps/dxc/bin/dxc", *extra, "-T", "cs_6_5", "-E", "main", "-O3", "-Fo", out/"probe.dxil",
                    ROOT/"guest/test_hwrt.hlsl"], check=True)
    exports = []
    programs = [(1286, "t1=32,u1=20") if intersection else (1280, "t1=32,u0=4"),
                (1237 if lumen_miss else 1282, "")]
    if intersection:
        programs.append((1283, ""))
    for which, (index, layout) in enumerate(programs):
        cmd = [ROOT/"build/host/frosty-shader", "rt-lift", utoc.with_suffix(".pak"), "Global", str(index), out, "6", layout]
        library = Path(subprocess.check_output(cmd, text=True).strip().strip('"'))
        if which == 2:
            name = next(l.split("\t")[1] for l in library.with_suffix(".exports").read_text().splitlines() if l.startswith("group\t"))
        else:
            stage = "raygeneration" if which == 0 else "miss"
            pattern = re.compile(r'\[shader\("'+stage+r'"\)\] void ([A-Za-z0-9_]+)\(')
            name = pattern.search(library.with_suffix(".hlsl").read_text())[1]
        exports.append(f"{library.name} {name}")
    (out/"occlusion.libs").write_text("\n".join(exports)+"\n")
    print(f"{ASSET}: {len(points)} vertices, {len(indices)//3} triangles, {hits} hits / {len(rays)-hits} misses; "
          f"fixture sha256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("utoc", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--lumen-miss", action="store_true", help="exercise the shipped Lumen material miss entry in the six-word probe")
    parser.add_argument("--intersection", action="store_true", help="check the title's closest-hit payload with the retail intersection raygen")
    parser.add_argument("--position-fetch", action="store_true", help="verify all three native object-space vertices against cooked indices")
    parser.add_argument("--shadow-launch", action="store_true", help="measure the shipped FOcclusionRGS viewport guard on a non-square ray dispatch")
    parser.add_argument("--shadow-rays", action="store_true", help="execute the complete FOcclusionRGS against an indexed cube shadow silhouette")
    a = parser.parse_args()
    fixture(a.utoc, a.out, a.lumen_miss, a.intersection, a.position_fetch)
    if a.shadow_launch:
        subprocess.run([ROOT/"build/host/frosty-shader","rt-guard",a.utoc.with_suffix(".pak"),"Global","3754",a.out],check=True)
    else:
        (a.out/"launch-guard.dxil").unlink(missing_ok=True)
    if a.shadow_rays:
        views="t0=float,t1=32,t2=float,t3=float,t4=float,t5=uint,t6=float,t7=float,t8=float,t9=uint,t11=uint,u0=float,u1=float"
        rows=[]
        for index,layout,stage in [(3754,views,"raygeneration"),(1282,"","miss"),(2671,"","closesthit")]:
            dll=Path(subprocess.check_output([ROOT/"build/host/frosty-shader","rt-lift",a.utoc.with_suffix(".pak"),"Global",str(index),a.out,"6",layout],text=True).strip().strip('"'))
            name=re.search(r'\[shader\("'+stage+r'"\)\] void (\w+)\(',dll.with_suffix(".hlsl").read_text())[1]
            rows.append(f"{dll.name} {name}")
        (a.out/"shadow.libs").write_text("\n".join(rows)+"\n")
        # Use a separate directory for the deliberate mismatch. It is not
        # accompanied by a production plan and cannot be selected by run.
        bad=Path(subprocess.check_output([ROOT/"build/host/frosty-shader","rt-lift-checked",a.utoc.with_suffix(".pak"),"Global","3754",
                                          a.out/"wrong-route","6",views,"0x20","0","0"],text=True).strip().strip('"'))
        shutil.copyfile(bad,a.out/"shadow-mismatch.dxil")
    else:
        (a.out/"shadow.libs").unlink(missing_ok=True)
        (a.out/"shadow-mismatch.dxil").unlink(missing_ok=True)
