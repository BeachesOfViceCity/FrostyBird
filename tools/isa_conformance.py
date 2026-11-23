#!/usr/bin/env python3
"""Assemble independent ISA tests with LLVM, lift to DXIL and write GPU readback oracles."""
import argparse
import math
import os
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MASK = 0xffffffff
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", type=Path, default=ROOT / "build/isa-conformance")
parser.add_argument("--compiler", type=Path, default=ROOT / "build/host/frosty-shader")
parser.add_argument("--image64",action="store_true",help="generate the typed 64-bit image-atomic fixture (SM 6.6)")
parser.add_argument("--gather-lod",action="store_true",help="generate an explicit-LOD gather fixture with real mip data")
parser.add_argument("--native-wave",action="store_true",help="test exact-size SM6.6 wave intrinsics in the compute interpreter")
parser.add_argument("--native-half",action="store_true",help="use native D3D half-vector arithmetic in the kernel fixtures")
parser.add_argument("--group-ordinal",action="store_true",help="test the Vulkan subgroup ordinal protocol for full physical waves")
parser.add_argument("--nested-exec",action="store_true",help="verify a masked diamond defining a VGPR inside a partial compute wave")
parser.add_argument("--large-cfg",action="store_true",help="verify two cyclic regions interleaved with forward EXEC branches")
parser.add_argument("--lane-spill",action="store_true",help="check a partly dead scalar pair spilled across fixed vector lanes")
parser.add_argument("--group-waves",action="store_true",help="exercise concurrent exact waves with cross-wave LDS and different barrier PCs")
parser.add_argument("--barrier-loops",action="store_true",help="verify literal and wave-dependent trip counts across workgroup barriers")
parser.add_argument("--mask-words",action="store_true",help="check numeric ballots, lane consumers and SCC after wave32 mask logic")
parser.add_argument("--queue-pc",action="store_true",help="check the large reference dispatcher with coherent UAV reads and returned atomics")
parser.add_argument("--subset-bits",action="store_true",help="check a proved 32-bit direction matrix and a one-bit near miss under a partial EXEC")
parser.add_argument("--legacy-product",action="store_true",help="check original DX9 zero-product, infinities, signed zero and NaNs")
parser.add_argument("--vm-order",action="store_true",help="check ordered UAV traffic across a conditional and a read-only VM wait")
parser.add_argument("--half-pairs",action="store_true",help="check native packed binary16 selectors, fused rounding and subnormals")
parser.add_argument("--buffer-producer",action="store_true",help="check cross-wave output atomics, with and without a mutable input")
parser.add_argument("--compare-literals",action="store_true",help="check FP32 comparisons with both immediate orderings, zeros, subnormals and NaNs")
parser.add_argument("--half-lds",action="store_true",help="check native 16-bit shared memory with mixed-width and unaligned accesses")
args = parser.parse_args()
if args.native_wave:os.environ["FROSTY_TEST_NATIVE_WAVE"]="1"
if args.native_half:os.environ["FROSTY_TEST_NATIVE_HALF"]="1"
if args.group_ordinal:os.environ["FROSTY_TEST_GROUP_ORDINAL"]="1"
args.out.mkdir(exist_ok=True, parents=True)
ops = re.findall(r'\{"([^"]+)",', (ROOT / "src/rdna2_catalog.inc").read_text())


class Kernel:
    def __init__(self, name, threads=64, wave=64, lds=0):
        self.name, self.threads, self.wave, self.lds = name, threads, wave, lds
        self.groups = 2
        self.rows = [[] for _ in range(threads * self.groups)]
        self.lines = ["s_mov_b32 s7, 0", "s_load_dwordx4 s[12:15], s[6:7], 0",
                      "s_waitcnt lgkmcnt(0)", f"v_mul_lo_u32 v27, {threads}, s8",
                      "v_add_nc_u32 v27, v27, v0"]
        self.vcc = "vcc" if wave == 64 else "vcc_lo"

    def store(self, reg, oracle):
        off = 4 * len(self.rows[0])
        self.lines.append(f"buffer_store_dword {reg}, v27, s[12:15], 0 idxen offset:{off}")
        for i, row in enumerate(self.rows):
            row.append(oracle(i) & MASK)

    def save(self):
        stride = 4 * len(self.rows[0])
        stem = args.out / self.name
        stem.with_suffix(".s").write_text("\n".join(self.lines + ["s_endpgm", ""]))
        subprocess.run(["llvm-mc", "-triple=amdgcn-amd-amdhsa", "-mcpu=gfx1030",
                        f"-mattr=+wavefrontsize{self.wave},-wavefrontsize{96-self.wave}",
                        "-filetype=obj", stem.with_suffix(".s"), "-o", stem.with_suffix(".o")], check=True)
        subprocess.run(["llvm-objcopy", "--only-section=.text", "-O", "binary",
                        stem.with_suffix(".o"), stem.with_suffix(".bin")], check=True)
        subprocess.run([args.compiler, "test-kernel", stem.with_suffix(".bin"), str(self.wave), str(self.threads),
                        str(self.lds), str(stride), stem.with_suffix(".dxil")], check=True)
        with stem.with_suffix(".ref").open("wb") as f:
            f.write(struct.pack("<3I", self.groups, self.threads, stride))
            for row in self.rows:
                f.write(struct.pack(f"<{len(row)}I", *row))


def comparison(t):
    width = int(t[1:])
    fp = t.startswith("f")
    mask = (1 << width) - 1
    sign = 1 << (width - 1)
    fmt = {16: "e", 32: "f", 64: "d"}[width]
    if fp:
        exponent = {16: 0x7c00, 32: 0x7f800000, 64: 0x7ff0000000000000}[width]
        quiet = {16: 1 << 9, 32: 1 << 22, 64: 1 << 51}[width]
        def enc(x):
            return int.from_bytes(struct.pack("<"+fmt, x), "little")
        pairs = [(enc(1), enc(2)), (enc(-1), enc(-2)), (0, sign), (sign | exponent, enc(-1)),
                 (exponent, exponent), (exponent | quiet, enc(1)), (enc(1), exponent | 1), (1, 0)]
        def num(x):
            return struct.unpack("<"+fmt, x.to_bytes(width//8, "little"))[0]
    else:
        pairs = [(0, 1), (mask, 0), (sign, mask), (sign-1, sign), (17, 17), (mask-1, mask),
                 (1, 0), ((1 << (width//2)) + 1, 1 << (width//2))]
        def num(x):
            return x - (1 << width) if t.startswith("i") and x & sign else x
    k = Kernel("compare-"+t)
    k.lines.append("v_and_b32 v28, 7, v0")
    for c, first in [(0, 2), (1, 4)]:
        for word in range(2 if width == 64 else 1):
            reg = f"v{first+word}"
            k.lines.append(f"v_mov_b32 {reg}, 0x{(pairs[0][c] >> (32*word)) & MASK:x}")
            for i in range(1, 8):
                k.lines.extend([f"v_mov_b32 v9, 0x{(pairs[i][c] >> (32*word)) & MASK:x}",
                                f"v_cmp_eq_u32 {k.vcc}, {i}, v28",
                                f"v_cndmask_b32 {reg}, {reg}, v9, {k.vcc}"])
    l, r = ("v[2:3]", "v[4:5]") if width == 64 else ("v2", "v4")
    def truth(pred, i):
        a, b = map(num, pairs[i % 8])
        unordered = math.isnan(a) or math.isnan(b) if fp else False
        return int({"f": False, "t": True, "tru": True, "lt": a < b, "le": a <= b, "gt": a > b,
                    "ge": a >= b, "eq": a == b, "ne": a != b, "lg": not unordered and a != b,
                    "neq": a != b, "nge": not a >= b, "nle": not a <= b, "nlt": not a < b,
                    "ngt": not a > b, "nlg": unordered or a == b, "u": unordered, "o": not unordered}[pred])
    for op in ops:
        m = re.fullmatch(r"v_cmp(x?)_(\w+)_"+t, op)
        if not m:
            continue
        cmpx, pred = m.groups()
        selectors = range(10) if pred == "class" else [None]
        for klass in selectors:
            rhs = r
            if klass is not None:
                k.lines.append(f"v_mov_b32 v9, {1<<klass}")
                rhs = "v9"
            if cmpx:
                k.lines.extend(["v_mov_b32 v10, 0", "s_mov_b64 s[16:17], exec", f"{op} {l}, {rhs}",
                                "v_mov_b32 v10, 1", "s_mov_b64 exec, s[16:17]"])
            else:
                k.lines.extend([f"{op} {k.vcc}, {l}, {rhs}", f"v_cndmask_b32 v10, 0, 1, {k.vcc}"])
            if klass is None:
                k.store("v10", lambda i: truth(pred, i))
            else:
                def selected(i):
                    a = pairs[i % 8][0]
                    mag = a & ~sign
                    c = (int(bool(a & quiet)) if mag > exponent else (2 if a & sign else 9) if mag == exponent
                         else (5 if a & sign else 6) if not mag else (4 if a & sign else 7)
                         if mag < (exponent & -exponent) else (3 if a & sign else 8))
                    return int(c == klass)
                k.store("v10", selected)
    k.save()


def shared(wave):
    k = Kernel(f"shared-wave{wave}", wave*2, wave, 8*wave)
    k.lines += ["v_lshlrev_b32 v1, 2, v0", "ds_write_b32 v1, v27", "s_waitcnt lgkmcnt(0)", "s_barrier",
                f"v_xor_b32 v2, {wave}, v0", "v_lshlrev_b32 v2, 2, v2", "ds_read_b32 v3, v2", "s_waitcnt lgkmcnt(0)"]
    k.store("v3", lambda i: i ^ wave)
    k.lines += [f"v_readlane_b32 s18, v27, {wave-1}", "v_mov_b32 v8, s18"]
    k.store("v8", lambda i: i // wave * wave + wave-1)
    k.lines += ["v_and_b32 v2, 1, v0", f"v_cmp_eq_u32 {k.vcc}, 1, v2", f"s_and_saveexec_b64 s[16:17], {k.vcc if wave==64 else 'vcc'}",
                "v_readfirstlane_b32 s18, v27", "s_mov_b64 exec, s[16:17]", "v_mov_b32 v8, s18"]
    k.store("v8", lambda i: i // wave * wave + 1)
    k.lines += ["v_mov_b32 v9, 1",
                "v_add_nc_u32_dpp v10, v27, v9 row_shr:1 row_mask:0xf bank_mask:0xf bound_ctrl:1"]
    k.store("v10", lambda i: i if i % 16 else 1)
    k.lines.append("v_add_nc_u32_dpp v10, v27, v9 dpp8:[1,0,3,2,5,4,7,6]")
    k.store("v10", lambda i: (i ^ 1) + 1)
    k.lines += ["s_mov_b32 s20, 0x76543210", "s_mov_b32 s21, 0xfedcba98",
                "v_permlanex16_b32 v10, v27, s20, s21"]
    k.store("v10", lambda i: i ^ 16)
    k.lines += ["ds_bpermute_b32 v10, v1, v27"]
    k.store("v10", lambda i: i)
    k.lines += ["v_mov_b32 v12, 0", "ds_read2_b64 v[4:7], v12 offset1:2"]
    for j,off in enumerate((0,1,4,5)):
        k.store(f"v{4+j}", lambda i: i//(2*wave)*(2*wave)+off)
    # Reuse the LDS allocation only after *both* guest waves finish reading
    # the old dwords. A barrier after the next write would be too late.
    k.lines += ["s_waitcnt lgkmcnt(0)", "s_barrier", "v_lshlrev_b32 v13, 1, v0", "v_or_b32 v14, 0x400, v27",
                "ds_write_b16 v13, v14", "s_barrier", "v_mov_b32 v15, 0x1234",
                "ds_read_u16_d16_hi v15, v13"]
    k.store("v15", lambda i: ((0x400|i)<<16)|0x1234)
    # Exchange an aligned 64-bit LDS value and preserve both original words.
    # All waves rendezvous before the per-lane exchange, with disjoint slots.
    k.lines += ["s_waitcnt lgkmcnt(0)", "s_barrier", "v_and_b32 v12, 1, v0", f"v_cmp_eq_u32 {k.vcc}, 0, v12",
                "s_and_saveexec_b64 s[16:17], vcc", "v_mov_b32 v20, v27", "v_mov_b32 v21, 0x87654321",
                "ds_write_b64 v1, v[20:21]", "s_mov_b64 exec, s[16:17]", "s_barrier",
                "v_mov_b32 v22, 0", "v_mov_b32 v23, 0", f"v_cmp_eq_u32 {k.vcc}, 0, v12",
                "s_and_saveexec_b64 s[16:17], vcc", "ds_wrxchg_rtn_b64 v[22:23], v1, v[20:21]", "s_mov_b64 exec, s[16:17]"]
    k.store("v22", lambda i: 0 if i&1 else i)
    k.store("v23", lambda i: 0 if i&1 else 0x87654321)
    k.save()


def packed():
    k=Kernel("packed-scalar")
    k.lines+=["s_setreg_imm32_b32 hwreg(HW_REG_MODE,2,2), 0","s_getreg_b32 s18, hwreg(HW_REG_MODE,2,2)",
              "v_mov_b32 v2, s18"]
    k.store("v2",lambda i:0)
    for bits,half in ((0x3f801000,0x3c00),(0x3f803000,0x3c02)):
        k.lines += [f"v_mov_b32 v2, 0x{bits:08x}", "v_cvt_f16_f32 v3, v2","v_and_b32 v3, 0xffff, v3"]
        k.store("v3",lambda i:half)
    k.lines += ["v_mov_b32 v1, 0x40003c00", "v_mov_b32 v2, 0x3c004200",
                "v_pk_add_f16 v3, v1, v2 op_sel:[1,0] op_sel_hi:[0,1]"]
    k.store("v3", lambda i: 0x40004500)  # (2+3,1+1)
    k.lines += ["v_mov_b32 v4, 0xdeadbeef", "v_xor_b32_sdwa v4, v1, v2 dst_sel:BYTE_2 dst_unused:UNUSED_PRESERVE src0_sel:BYTE_1 src1_sel:BYTE_1"]
    k.store("v4", lambda i: (0xdeadbeef & 0xff00ffff) | ((0x3c ^ 0x42) << 16))
    k.lines += ["s_mov_b32 s18, -1", "s_add_u32 s18, s18, 1", "s_addc_u32 s19, s18, 0", "v_mov_b32 v5, s19"]
    k.store("v5", lambda i: 1)
    k.lines += ["s_sub_u32 s18, s18, 1", "s_subb_u32 s19, 9, 4", "v_mov_b32 v5, s19"]
    k.store("v5", lambda i: 4)
    k.lines += ["s_mov_b32 s20, 0x89abcdef", "s_mov_b32 s21, 0x76543210", "s_lshr_b64 s[22:23], s[20:21], 36",
                "v_mov_b32 v6, s22", "v_mov_b32 v7, s23"]
    k.store("v6", lambda i: 0x7654321)
    k.store("v7", lambda i: 0)
    k.lines += ["v_cvt_f64_u32 v[6:7], v27", "v_add_f64 v[6:7], v[6:7], 1.0",
                "v_cvt_u32_f64 v8, v[6:7]"]
    k.store("v8", lambda i: i+1)
    k.save()


def binary64():
    k=Kernel("binary64-soft")
    tiny=math.ulp(0.)
    x=[1+2**-52, 2**-1022, tiny, -0., -17.25, 2**500, 0.5, 1.5]
    y=[1-2**-52, .5, 2., -1., 2.75, 2**-400, -7., -1.25]
    z=[-1., -tiny, -tiny, -0., 47.4375, -(2**100), 3.5, 2.0]
    enc=lambda v: int.from_bytes(struct.pack("<d",v),"little")
    k.lines.append("v_and_b32 v28, 7, v0")
    for values,first in ((x,2),(y,4),(z,6)):
        for j in (0,1):
            reg=f"v{first+j}"
            k.lines.append(f"v_mov_b32 {reg}, 0x{(enc(values[0])>>(32*j))&MASK:x}")
            for n in range(1,8):
                k.lines.extend([f"v_mov_b32 v9, 0x{(enc(values[n])>>(32*j))&MASK:x}",
                                f"v_cmp_eq_u32 {k.vcc}, {n}, v28",f"v_cndmask_b32 {reg}, {reg}, v9, {k.vcc}"])
    def output(op,fn):
        k.lines.append(op)
        for j in (0,1):
            k.store(f"v{10+j}",lambda i: enc(fn(x[i%8],y[i%8],z[i%8]))>>(32*j))
    output("v_add_f64 v[10:11], v[2:3], v[4:5]",lambda a,b,c:a+b)
    output("v_mul_f64 v[10:11], v[2:3], v[4:5]",lambda a,b,c:a*b)
    # The first case rounds to zero if the multiply and add are not fused.
    output("v_fma_f64 v[10:11], v[2:3], v[4:5], v[6:7]",math.fma)
    output("v_rcp_f64 v[10:11], v[2:3]",lambda a,b,c:1./a if a else math.copysign(math.inf,a))
    output("v_sqrt_f64 v[10:11], |v[2:3]|",lambda a,b,c:math.sqrt(abs(a)))
    k.save()


def subword_memory():
    k=Kernel("subword-buffer")
    k.lines += ["v_mov_b32 v2, 0x01234567", "v_mov_b32 v3, 0xe901", "v_mov_b32 v4, 0x00b60000"]
    k.store("v2",lambda i: 0xe901b667)
    k.lines += ["buffer_store_short v3, v27, s[12:15], 0 idxen offset:2",
                "buffer_store_byte_d16_hi v4, v27, s[12:15], 0 idxen offset:1",
                "s_waitcnt_vscnt null, 0",
                "buffer_load_sbyte v5, v27, s[12:15], 0 idxen offset:1",
                "v_mov_b32 v6, 0xa1200000", "buffer_load_short_d16 v6, v27, s[12:15], 0 idxen offset:2",
                "s_waitcnt vmcnt(0)"]
    k.store("v5",lambda i: 0xffffffb6)
    k.store("v6",lambda i: 0xa120e901)
    k.lines += ["s_mov_b32 s20, 0x80000001", "s_bitreplicate_b64_b32 s[22:23], s20",
                "v_mov_b32 v5, s22", "v_mov_b32 v6, s23", "s_quadmask_b64 s[24:25], s[22:23]", "v_mov_b32 v7, s24"]
    for j,v in enumerate((3,0xc0000000,0x8001)):
        k.store(f"v{5+j}",lambda i:v)
    k.lines += ["v_mov_b32 v20, v27", "v_mov_b32 v21, 0x76543210", "s_mov_b32 m0, s8",
                "v_movrels_b32 v5, v20"]
    k.store("v5",lambda i: i if i<k.threads else 0x76543210)
    k.save()


def image64():
    k=Kernel("visibility-max64",threads=32,wave=32)
    width,height=37,k.groups
    seeds=[(1<<32)|(0xf0000000+x+width*y) for y in range(height) for x in range(width)]
    expected=seeds.copy()
    k.lines += ["s_load_dwordx8 s[16:23], s[6:7], 32","s_waitcnt lgkmcnt(0)",
                "v_mov_b32 v1, s8","v_not_b32 v4, v27","v_add_nc_u32 v5, 0x80000000, v0",
                "v_mov_b32 v6, v4","v_mov_b32 v7, v5",
                "image_atomic_umax v[4:5], v[0:1], s[16:23] dmask:3 dim:SQ_RSRC_IMG_2D unorm glc",
                "s_waitcnt vmcnt(0)"]
    # Every lane has its own first texel: both words of the returned OLD
    # value are deterministic and distinct from the submitted candidate.
    k.store("v4",lambda i:seeds[(i//32)*width+i%32])
    k.store("v5",lambda i:seeds[(i//32)*width+i%32]>>32)
    k.lines+=["v_and_b32 v2, 3, v0","v_add_nc_u32 v2, 32, v2","v_mov_b32 v3, s8",
              "image_atomic_umax v[6:7], v[2:3], s[16:23] dmask:3 dim:SQ_RSRC_IMG_2D unorm"]
    for i in range(32*height):
        y,x=divmod(i,32);candidate=((0x80000000+x)<<32)|(MASK-i)
        expected[y*width+x]=candidate
        j=y*width+32+x%4;expected[j]=max(expected[j],candidate)
    k.save()
    (args.out/"visibility-max64.img64").write_bytes(struct.pack("<II",width,height)+
        struct.pack(f"<{len(seeds)}Q",*seeds)+struct.pack(f"<{len(expected)}Q",*expected))

def prefix_mask():
    k=Kernel("prefix-mask64")
    k.lines+=["v_and_b32 v1, 1, v0","v_cmp_ne_u32 vcc, 0, v1",
              "s_and_saveexec_b64 s[16:17], vcc","s_mov_b64 s[18:19], exec",
              "s_mov_b64 exec, s[16:17]","v_mbcnt_lo_u32_b32 v2, s18, 0",
              "v_mbcnt_hi_u32_b32 v2, s19, v2"]
    k.store("v2",lambda i:(i%64)//2)
    k.lines+=["v_xor_b32 v1, 0x1badcafe, v27","v_not_b32 v2, v1",
              "v_mbcnt_lo_u32_b32 v3, v1, 11","v_mbcnt_hi_u32_b32 v3, v2, v3"]
    def count(i):
        lo=0x1badcafe^i;word=((MASK^lo)<<32)|lo
        return 11+sum((word>>j)&1 for j in range(i%64))
    k.store("v3",count);k.save()

def scalar_literal():
    k=Kernel("scalar-pc-table")
    k.lines+=["s_getpc_b64 s[20:21]",".Llitpc:","s_add_u32 s20, s20, .Ltable-.Llitpc",
              "s_addc_u32 s21, s21, 0","s_lshl_b32 s22, s8, 3","s_mov_b32 s23, 0",
              "s_add_u32 s22, s22, s20","s_addc_u32 s23, s23, s21",
              "s_load_dwordx2 s[16:17], s[22:23], 0","s_waitcnt lgkmcnt(0)","v_mov_b32 v2, s16","v_mov_b32 v3, s17"]
    k.store("v2",lambda i:0xface1274 if i<k.threads else 0xbadf00d)
    k.store("v3",lambda i:0xc0012026 if i<k.threads else 0x12345678)
    k.lines+=["s_endpgm",".Ltable:",".long 0xface1274, 0xc0012026, 0xbadf00d, 0x12345678"]
    k.save()

def vcc_words():
    k=Kernel("vcc-words32",threads=64,wave=32)
    k.lines+=["v_and_b32 v1, 7, v0","v_cmp_eq_u32 vcc_lo, 3, v1",
              "s_and_saveexec_b32 s20, vcc_lo","v_readfirstlane_b32 vcc_lo, v27",
              "s_mov_b32 s19, vcc_lo","s_mov_b32 exec_lo, s20","v_mov_b32 v2, s19"]
    k.store("v2",lambda i:i//32*32+3)
    k.lines+=["s_mov_b32 vcc_hi, 0x81234567","s_mov_b32 vcc_lo, 0xa5719335",
              "v_mov_b32 v4, vcc_hi","v_cndmask_b32 v3, 17, 23, vcc_lo"]
    k.store("v3",lambda i:23 if (0xa5719335>>(i%32))&1 else 17)
    k.store("v4",lambda i:0x81234567)
    k.save()

def mask_edge(wave):
    k=Kernel("mask-edge"+str(wave),threads=64,wave=wave,lds=4)
    w=str(wave);cc=k.vcc;ex="exec" if wave==64 else "exec_lo"
    k.lines += ["s_load_dwordx8 s[16:23], s[6:7], 64","s_waitcnt lgkmcnt(0)",
                "v_mov_b32 v10, -1","s_cmp_eq_u32 s8, 0","s_cbranch_scc1 test_mask",
                "v_mov_b32 v10, 0","s_mov_b32 s16, 89","s_branch test_mask","test_mask:",
                f"v_cmp_eq_u32 {cc}, -1, v10","s_mov_b32 s19, 101",
                f"s_andn2_b{w} {cc}, {ex}, {cc}","s_cbranch_vccnz use_index",
                "s_mov_b32 s16, 37","use_index:","v_mov_b32 v2, s16","v_mov_b32 v3, s19"]
    k.store("v2",lambda i:37 if i<64 else 89)
    k.store("v3",lambda i:101)
    # Restoring EXEC brings lanes with a different value back into the vote.
    save="s[20:21]" if wave==64 else "s20"
    k.lines += ["v_mov_b32 v10, 0","v_and_b32 v9, 1, v0",f"v_cmp_eq_u32 {cc}, 0, v9",
                f"s_and_saveexec_b{w} {save}, {cc}","v_mov_b32 v10, -1",
                "s_cmp_eq_u32 s8, 0","s_cbranch_scc1 restore","v_mov_b32 v10, 0","restore:",
                f"s_mov_b{w} {ex}, {save}",f"v_cmp_eq_u32 {cc}, -1, v10",
                f"s_andn2_b{w} {cc}, {ex}, {cc}","s_cbranch_vccnz any_zero",
                "v_mov_b32 v2, 0","s_branch exit_mask","any_zero:","v_mov_b32 v2, 1","exit_mask:"]
    k.store("v2",lambda i:1)
    k.save()

def nested_exec():
    k=Kernel("nested-exec",threads=16,wave=64)
    k.lines+=["v_mov_b32 v4, 91","v_cmp_lt_u32 vcc, v0, 14","s_and_saveexec_b64 s[18:19], vcc",
              "s_cbranch_execz outer_done","v_and_b32 v2, 1, v0","v_cmp_eq_u32 vcc, 0, v2",
              "s_and_saveexec_b64 s[20:21], vcc","s_cbranch_execz otherwise","v_add_nc_u32 v29, 23, v27",
              "otherwise:","s_andn2_b64 exec, s[20:21], exec","s_cbranch_execz inside_done",
              "v_mul_lo_u32 v29, 5, v27","inside_done:","s_mov_b64 exec, s[20:21]",
              "v_xor_b32 v4, 11, v29","outer_done:","s_mov_b64 exec, s[18:19]"]
    k.store("v4",lambda i:91 if i%16>=14 else 11^(i*5 if i%2 else i+23))
    k.save()

def lane_spill():
    k=Kernel("fixed-lane-spill")
    k.lines+=["s_mov_b32 s40, s6","v_writelane_b32 v72, s40, 2","v_writelane_b32 v72, s41, 3",
              "s_mov_b32 s40, 7","s_mov_b32 s41, 21","v_readlane_b32 s40, v72, 2",
              "v_readlane_b32 s41, v72, 3","s_mov_b32 s41, 0","s_load_dwordx4 s[12:15], s[40:41], 0",
              "s_waitcnt lgkmcnt(0)","v_xor_b32 v4, 0x215, v27"]
    k.store("v4",lambda i:0x215^i);k.save()

def group_waves(wave,dynamic):
    # One group shares an LDS allocation between two separately branching
    # guest waves. Every output depends on the *other* wave's contents.
    k=Kernel(f"group-{wave}-{'loop' if dynamic else 'linear'}",wave*2,wave,8*wave)
    k.lines+=["v_lshlrev_b32 v1, 2, v0",f"v_xor_b32 v2, {wave}, v0","v_lshlrev_b32 v2, 2, v2",
              "ds_write_b32 v1, v27","s_waitcnt lgkmcnt(0)","s_barrier",
              "ds_read_b32 v3, v2","s_waitcnt lgkmcnt(0)","s_barrier"]
    k.store("v3",lambda i:i^wave)
    k.lines+=["v_readfirstlane_b32 s18, v0","s_mov_b32 s22, 2","phase_loop:",
              "v_add_nc_u32 v3, 7, v3","ds_write_b32 v1, v3","s_waitcnt lgkmcnt(0)"]
    if dynamic:
        k.lines+=[f"s_cmp_ge_u32 s18, {wave}","s_cbranch_scc1 odd_barrier","s_barrier",
                  "s_branch both_ready","odd_barrier:","s_barrier","both_ready:"]
    else:k.lines+=["s_barrier"]
    k.lines+=["ds_read_b32 v3, v2","s_waitcnt lgkmcnt(0)","s_barrier"]
    if dynamic:
        k.lines+=["s_sub_u32 s22, s22, 1","s_cmp_lg_u32 s22, 0","s_cbranch_scc1 phase_loop"]
    k.store("v3",lambda i:(i^wave)+14 if dynamic else i+7)
    if dynamic:
        k.lines+=[f"s_cmp_lt_u32 s18, {wave}","s_cbranch_scc1 still_active","s_endpgm","still_active:","s_barrier"]
        k.store("v27",lambda i:i if i%(wave*2)<wave else 0xa7a7a7a7)
    k.save()

def nested_loops(wave):
    k=Kernel(f"natural-loops-{wave}",threads=2*wave,wave=wave)
    k.lines+=["v_readfirstlane_b32 s26, v0",f"s_lshr_b32 s26, s26, {5 if wave==32 else 6}",
              "v_mov_b32 v4, v27","s_mov_b32 s18, 0","outer:","s_mov_b32 s19, 0","inner:",
              "s_cmp_eq_u32 s19, s26","s_cbranch_scc1 next_inner","s_cmp_lg_u32 s18, 1","s_cbranch_scc1 normal",
              "s_cmp_eq_u32 s19, 2","s_cbranch_scc1 leave_inner","normal:","s_mul_i32 s20, s18, 11",
              "s_add_i32 s20, s20, s19","v_add_nc_u32 v4, s20, v4","next_inner:",
              "s_add_i32 s19, s19, 1","s_cmp_lt_u32 s19, 4","s_cbranch_scc1 inner","leave_inner:",
              "s_cmp_lg_u32 s26, 1","s_cbranch_scc1 next_outer","s_cmp_eq_u32 s18, 2","s_cbranch_scc1 done",
              "next_outer:","s_add_i32 s18, s18, 1","s_cmp_lt_u32 s18, 5","s_cbranch_scc1 outer","done:"]
    def result(i):
        v=i;w=i//wave%2
        for outer in range(5):
            for inner in range(4):
                if inner==w:continue
                if outer==1 and inner==2:break
                v+=outer*11+inner
            if w==1 and outer==2:break
        return v
    k.store("v4",result);k.save()

def barrier_loop(wave,dependent):
    k=Kernel(f"barrier-{wave}-{'wave-limit' if dependent else 'literal'}",threads=2*wave,wave=wave,lds=8*wave)
    k.lines += ["v_mov_b32 v3, v27","v_lshlrev_b32 v1, 2, v0",f"v_xor_b32 v2, {wave}, v0",
                "v_lshlrev_b32 v2, 2, v2","s_mov_b32 s24, 1","s_mov_b32 s25, 7"]
    if dependent:
        # Both waves take three trips, but the compiler cannot replace this
        # dynamic limit with the earlier literal. Their elected values differ.
        k.lines += ["v_readfirstlane_b32 s25, v0",f"s_lshr_b32 s25, s25, {5 if wave==32 else 6}",
                    "s_add_u32 s25, s25, 6"]
    k.lines += ["again:","v_add_nc_u32 v3, s24, v3","ds_write_b32 v1, v3",
                "s_waitcnt lgkmcnt(0)","s_barrier","ds_read_b32 v3, v2","s_lshl_b32 s24, s24, 1",
                "s_cmp_gt_u32 s24, s25","s_waitcnt lgkmcnt(0)","s_barrier","s_cbranch_scc0 again"]
    k.store("v3",lambda i:(i^wave)+7);k.save()
    lowered=(args.out/(k.name+".hlsl")).read_text()
    if ("common barrier sequence" in lowered)!=(args.native_wave and not dependent):raise ValueError("incorrect barrier proof")

def mask_words():
    k=Kernel("mask-word-uses",threads=32,wave=32,lds=4)
    k.lines += ["v_and_b32 v1, 3, v0","v_cmp_eq_u32 s18, 0, v1","v_cmp_eq_u32 vcc_lo, 2, v1",
                "s_or_b32 s18, s18, vcc_lo","s_xor_b32 s19, s18, -1","s_cselect_b32 s21, 75, 11",
                "v_cndmask_b32 v2, 12, 42, s18","v_mov_b32 v3, s19","v_mov_b32 v4, s21",
                "s_and_saveexec_b32 s22, s19","v_cmp_eq_u32 s20, 1, v1","s_andn2_b32 s23, exec_lo, s20",
                "s_and_b32 s20, s20, s23","s_cselect_b32 s24, 99, 25","s_mov_b32 exec_lo, s22",
                "v_cndmask_b32 v5, 17, 63, s23","v_mov_b32 v6, s24","s_mov_b32 s18, 0x80000001",
                "v_cndmask_b32 v7, 16, 41, s18"]
    for r,expected in (("v2",lambda i:12 if i&1 else 42),("v3",lambda _:0xaaaaaaaa),("v4",lambda _:75),
                       ("v5",lambda i:63 if i%4==3 else 17),("v6",lambda _:25),("v7",lambda i:41 if i%32 in (0,31) else 16)):
        k.store(r,expected)
    k.save()

def queue_pc(wave):
    k=Kernel(f"queue-pc-{wave}",threads=wave,wave=wave,lds=wave*4)
    k.lines+=["v_mov_b32 v3, 3","buffer_atomic_add v3, v27, s[12:15], 0 idxen glc","s_waitcnt vmcnt(0)",
              "buffer_load_dword v4, v27, s[12:15], 0 idxen glc","s_waitcnt vmcnt(0)",
              "v_and_b32 v9, 7, v0","s_mov_b32 s22, 3","loop:"]
    expected=[0xa7a7a7aa for _ in k.rows]
    for j in range(48):
        a=j%8;v=1+3*j
        k.lines += [f"v_cmp_eq_u32 {k.vcc}, {a}, v9",f"s_and_saveexec_b{wave} {'s[18:19]' if wave==64 else 's18'}, {k.vcc}",
                    f"s_cbranch_execz join_{j}",f"v_add_nc_u32 v4, {v}, v4",f"join_{j}:",
                    f"s_mov_b{wave} {'exec' if wave==64 else 'exec_lo'}, {'s[18:19]' if wave==64 else 's18'}"]
        for i in range(len(expected)):
            if i%8==a:expected[i]+=3*v
    k.lines+=["s_barrier","s_sub_u32 s22, s22, 1","s_cmp_lg_u32 s22, 0","s_cbranch_scc1 loop"]
    k.rows=[[0xa7a7a7aa] for _ in k.rows]
    k.store("v3",lambda i:0xa7a7a7a7);k.store("v4",lambda i:expected[i]);k.save()
    h=(args.out/(k.name+".hlsl")).read_text()
    if args.native_wave and ("if(wave_pc<" not in h or "DeviceMemoryBarrier();" not in h):raise ValueError("expected fenced PC dispatcher")

def subset_bits(near):
    k=Kernel("subset-"+("near" if near else "exact"),threads=32,wave=32,lds=4);k.groups=8;k.rows=[[] for _ in range(256)]
    k.lines+=["v_mul_lo_u32 v12, 0x9e3779b9, v27","v_xor_b32 v12, 0xb2751843, v12",
              "v_mov_b32 v4, 76","v_and_b32 v14, 3, v0","v_cmp_ne_u32 vcc_lo, 0, v14","s_and_saveexec_b32 s18, vcc_lo",
              "v_and_b32 v9, 1, v12","v_ashrrev_i32 v10, 31, v12","v_xor_b32 v4, v9, v10"]
    masks=[sum(1<<j for j in range(32) if j&~i==0) for i in range(32)]
    if near:masks[19]^=1<<11
    for i in range(1,31):
        k.lines+=[f"v_bfe_u32 v11, v12, {i}, 1","v_sub_nc_u32 v11, 0, v11",f"v_and_b32 v11, {masks[i]:#x}, v11"]
        if i==30:k.lines+=["v_mov_b32 v12, 22","v_xad_u32 v4, v4, v11, 0xd97dce6b"]
        else:k.lines+=["v_xor_b32 v4, v4, v11"]
    k.lines+=["s_mov_b32 exec_lo, s18"]
    def oracle(i):
        if not i%4:return 76
        n=(i*0x9e3779b9 & MASK)^0xb2751843;v=0
        for bit,mask in enumerate(masks):
            if n>>bit&1:v^=mask
        return v+0xd97dce6b
    k.store("v4",oracle);k.save();h=(args.out/(k.name+".hlsl")).read_text()
    if ("xs_subset_bits(xs_bit_input_" in h)!=(args.native_wave and not near):raise ValueError("incorrect direction-matrix proof")

def legacy_product():
    k=Kernel("legacy-product",wave=32,threads=32)
    pairs=[(0x80000000,0xbf800000),(0xff800000,0),(0x7fc12345,0x80000000),(0x3f800000,0x7fc00123),
           (0x3fc00000,0xc0000000),(0,0x7fc54321),(0x7fc00000,0x7fc00000),(0x7f800000,0xc0000000)]
    k.lines+=["v_and_b32 v8, 7, v0","v_mbcnt_lo_u32_b32 v9, -1, 0"] # enter the exact wave path
    for j,(x,y) in enumerate(pairs):
        if not j:k.lines += [f"v_mov_b32 v10, {x:#x}",f"v_mov_b32 v11, {y:#x}"];continue
        k.lines += [f"v_cmp_eq_u32 vcc_lo, {j}, v8",f"v_mov_b32 v12, {x:#x}","v_cndmask_b32 v10, v10, v12, vcc_lo",
                    f"v_mov_b32 v12, {y:#x}","v_cndmask_b32 v11, v11, v12, vcc_lo"]
    k.lines+=["v_mul_legacy_f32 v12, v10, v11","v_cmp_u_f32 vcc_lo, v12, v12","v_mov_b32 v13, 0x7fc00000",
              "v_cndmask_b32 v12, v12, v13, vcc_lo"]
    k.store("v12",lambda i:[0,0,0,0x7fc00000,0xc0400000,0,0x7fc00000,0xff800000][i%8]);k.save()

def vm_order():
    k=Kernel("vm-order",threads=64,wave=64)
    k.lines+=["v_mbcnt_lo_u32_b32 v9, -1, 0","s_waitcnt vmcnt(0)","s_cmp_eq_u32 s8, 0","s_cbranch_scc1 read_final",
              "v_mov_b32 v2, 0x50000a50","buffer_atomic_or v2, v27, s[12:15], 0 idxen","read_final:",
              "s_waitcnt vmcnt(0)","buffer_load_dword v3, v27, s[12:15], 0 idxen","s_waitcnt vmcnt(0)"]
    k.store("v3",lambda i:0xa7a7a7a7 | (0x50000a50 if i>=64 else 0));k.save()
    if args.native_wave:
        h=(args.out/"vm-order.hlsl").read_text()
        defs=re.findall(r"^#define XS_DEVICE_WAIT_\d+\(\) (.*)$",h,re.M)
        if "" not in defs or defs.count("DeviceMemoryBarrier()")!=2:raise ValueError(f"unexpected VM fence set {defs}")

def half_pairs():
    k=Kernel("fp16-pairs",threads=64,wave=64)
    vals=[(0x04003c01,0x38003bfe,0x0000bc00),(0xb800bc00,0x40004000,0x38003c00),
          (0x81000400,0x3c003800,0x80000000),(0x3e00c100,0xb8004200,0x4000bc00)]
    k.lines+=["v_and_b32 v2, 3, v0"]
    for j in range(3):
        k.lines.append(f"v_mov_b32 v{3+j}, {vals[0][j]:#x}")
        for i in range(1,4):k.lines+=[f"v_cmp_eq_u32 vcc, {i}, v2",f"v_mov_b32 v9, {vals[i][j]:#x}",f"v_cndmask_b32 v{3+j}, v{3+j}, v9, vcc"]
    def half(bits,hi):return struct.unpack("<e",((bits>>(16*hi))&65535).to_bytes(2,"little"))[0]
    def pair(a,b):return int.from_bytes(struct.pack("<ee",a,b),"little")
    k.lines+=["v_pk_fma_f16 v6, v3, v4, v5"]
    k.store("v6",lambda i:pair(*(math.fma(*(half(v,h) for v in vals[i%4])) for h in (0,1))))
    k.lines+=["v_pk_mul_f16 v6, v3, v4 op_sel:[1,0] op_sel_hi:[0,1] neg_lo:[1,0]"]
    k.store("v6",lambda i:pair(-half(vals[i%4][0],1)*half(vals[i%4][1],0),half(vals[i%4][0],0)*half(vals[i%4][1],1)))
    k.lines+=["v_pk_max_f16 v6, v3, v4"]
    k.store("v6",lambda i:pair(*(max(half(vals[i%4][0],h),half(vals[i%4][1],h)) for h in (0,1))))
    k.save()

def compare_literals():
    k=Kernel("fp32-literals",wave=32,threads=32)
    vals=[0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,0x7f800000,0xff800000,0x7f800001,0xffc01515,
          0x3f800000,0xbf800000,0x00800000,0x80800000,0x41200000,0xc1200000]
    k.lines+=["v_and_b32 v2, 15, v0","v_mov_b32 v3, 0"]
    for i,num in enumerate(vals[1:],1):
        k.lines+=[f"v_cmp_eq_u32 vcc_lo, {i}, v2",f"v_mov_b32 v4, {num:#x}","v_cndmask_b32 v3, v3, v4, vcc_lo"]
    f=lambda bits:struct.unpack("<f",struct.pack("<I",bits))[0]
    def rel(a,b):return 8 if math.isnan(a) or math.isnan(b) else 1 if a<b else 4 if a>b else 2
    for r in [0,0x80000000,0x3f800000,1,0x7f800000,0x7f800010]:
        for flip in (False,True):
            k.lines+=["v_mov_b32 v5, 0"]
            for mask,op in enumerate(("f","lt","eq","le","gt","lg","ge","o","u","nge","nlg","ngt","nle","neq","nlt","tru")):
                a,b=("v3",f"{r:#x}") if not flip else (f"{r:#x}","v3")
                k.lines+=[f"v_cmp_{op}_f32 vcc_lo, {a}, {b}",f"v_mov_b32 v6, {1<<mask}",
                          "v_cndmask_b32 v6, 0, v6, vcc_lo","v_or_b32 v5, v5, v6"]
            k.store("v5",lambda i:sum(1<<m for m in range(16) if m&(rel(f(r),f(vals[i%16])) if flip else rel(f(vals[i%16]),f(r)))))
    k.save()

def buffer_producer(mutable):
    k=Kernel("producer-"+("feedback" if mutable else "srv"),threads=128,wave=64,lds=512)
    k.lines+=["v_lshlrev_b32 v1, 2, v0","v_xor_b32 v2, 64, v0","v_lshlrev_b32 v2, 2, v2",
              "s_load_dwordx4 s[16:19], s[6:7], 128","s_waitcnt lgkmcnt(0)",
              f"buffer_load_dword v3, v27, s[{'12:15' if mutable else '16:19'}], 0 idxen","s_waitcnt vmcnt(0)",
              "ds_write_b32 v1, v3","s_waitcnt lgkmcnt(0)","s_barrier",
              "ds_read_b32 v4, v2","s_waitcnt lgkmcnt(0)","v_mov_b32 v5, 0x00ac0051",
              "buffer_atomic_or v5, v27, s[12:15], 0 idxen","s_waitcnt vmcnt(0)"]
    k.rows=[[0xa7a7a7a7 | 0x00ac0051] for _ in k.rows]
    value=lambda i:(i*0x15d+0x712358)&MASK
    k.store("v4",lambda i:0xa7a7a7a7 if mutable else value(i^64))
    k.save()
    if not mutable:(args.out/(k.name+".srv")).write_bytes(b"".join(struct.pack("<I",value(i)) for i in range(len(k.rows))))
    if args.native_wave:
        h=(args.out/(k.name+".hlsl")).read_text()
        if ("xs_wave_alloc" in h or "uint xs_group_id()" in h)==mutable:raise ValueError("wrong scheduler for "+k.name)

def counted_output(poll):
    k=Kernel("output-"+("polled" if poll else "counted"),threads=128,wave=64,lds=512)
    k.lines+=["v_lshlrev_b32 v1, 2, v0","ds_write_b32 v1, v27","s_barrier","s_mov_b32 s18, 0",
              "v_mov_b32 v4, 0","again:","buffer_load_dword v3, v27, s[12:15], 0 idxen","s_waitcnt vmcnt(0)",
              "v_xor_b32 v4, v3, v4","s_add_u32 s18, s18, 1"]
    if poll:
        # A bounded fixture at run time, but the compiler cannot assume the
        # condition loaded from the UAV remains true/false across iterations.
        k.lines+=["v_cmp_eq_u32 vcc, 0, v3","s_cbranch_vccnz again"]
    else:k.lines+=["s_cmp_lt_u32 s18, 3","s_cbranch_scc1 again"]
    k.lines+=["v_mov_b32 v5, 0xff0fff0f","buffer_atomic_and v5, v27, s[12:15], 0 idxen",
              "v_mov_b32 v5, 0x10000100","buffer_atomic_or v5, v27, s[12:15], 0 idxen","s_waitcnt vmcnt(0)"]
    k.rows=[[(0xa7a7a7a7&0xff0fff0f)|0x10000100] for _ in k.rows]
    k.store("v4",lambda i:0xa7a7a7a7);k.save()
    if args.native_wave:
        h=(args.out/(k.name+".hlsl")).read_text()
        if ("xs_wave_alloc" in h or "uint xs_group_id()" in h)!=(args.group_ordinal and not poll):raise ValueError("wrong counted output proof")

def half_lds(w,odd):
    k=Kernel(f"half-lds-{w}-{'unaligned' if odd else 'aligned'}",threads=w,wave=w,lds=8*w)
    k.lines+=["v_lshlrev_b32 v1, 3, v0","v_xor_b32 v12, 0x274ec398, v27","v_xor_b32 v13, 0x81ad320c, v27",
              "ds_write2_b32 v1, v12, v13 offset1:1","s_waitcnt lgkmcnt(0)",
              "ds_read2_b32 v[14:15], v1 offset1:1","s_waitcnt lgkmcnt(0)"]
    k.store("v14",lambda i:0x274ec398^i);k.store("v15",lambda i:0x81ad320c^i)
    k.lines+=["v_lshlrev_b32 v2, 1, v0"]
    for j in range(12):
        k.lines+=[f"v_add_nc_u32 v3, {j+33}, v27","ds_write_b16 v2, v3"]
    k.lines+=["s_waitcnt lgkmcnt(0)","v_and_b32 v4, -4, v2","ds_read_b32 v5, v4","s_waitcnt lgkmcnt(0)"]
    k.store("v5",lambda i:(44+(i&~1))|((45+(i&~1))<<16))
    k.lines+=["v_xor_b32 v6, 2, v2","v_mov_b32 v7, 0x34100237","ds_read_u16_d16_hi v7, v6","s_waitcnt lgkmcnt(0)"]
    k.store("v7",lambda i:0x237|((44+(i^1))<<16))
    k.lines+=["v_lshlrev_b32 v8, 2, v0","v_mov_b32 v9, 0xa1234567","ds_write_b32 v8, v9","s_waitcnt lgkmcnt(0)",
              f"v_or_b32 v10, {1 if odd else 0}, v8","ds_write_b16 v10, v3","s_waitcnt lgkmcnt(0)","ds_read_b32 v11, v8","s_waitcnt lgkmcnt(0)"]
    k.store("v11",lambda i:0xa1000067|((44+i)<<8) if odd else 0xa1230000|(44+i));k.save()
    if args.native_wave and args.native_half and ("groupshared uint16_t xs_half_lds" in (args.out/(k.name+".hlsl")).read_text())==odd:
        raise ValueError("wrong native narrow LDS eligibility")

def large_cfg():
    k=Kernel("large-cyclic",threads=64,wave=64,lds=4)
    k.lines+=["v_mov_b32 v4, v27","v_and_b32 v9, 7, v0"]
    expected=list(range(2*k.threads))
    for phase,(reps,segments) in enumerate(((2,135),(1,31),(3,135))):
        if reps>1:k.lines += [f"s_mov_b32 s20, {reps}",f"loop_{phase}:"]
        for j in range(segments):
            label=f"merge_{phase}_{j}";arg=(j*3+1)&7;constant=5+phase+j
            k.lines += [f"v_cmp_eq_u32 vcc, {arg}, v9","s_and_saveexec_b64 s[16:17], vcc",f"s_cbranch_execz {label}",
                        f"{'v_xor_b32' if phase==2 else 'v_add_nc_u32'} v4, {constant}, v4",label+":","s_mov_b64 exec, s[16:17]"]
            for lane in range(len(expected)):
                if lane%8==arg:
                    if phase==2:expected[lane]^=constant
                    else:expected[lane]+=reps*constant
        if reps>1:k.lines += ["s_sub_u32 s20, s20, 1","s_cmp_lg_u32 s20, 0",f"s_cbranch_scc1 loop_{phase}"]
    k.store("v4",lambda i:expected[i]);k.save()

def gather_lod():
    k=Kernel("gather-lod",threads=16,wave=32)
    w,h,levels=12,9,4
    u=[-.125,.35,.875,1.25];v=[-.25,.55,.88,1.25];lod=[-1.,0.,.75,4.]
    k.lines+=["v_and_b32 v14, 3, v0","v_lshrrev_b32 v15, 2, v0"]
    for vals,reg,index in ((u,"v1","v14"),(v,"v2","v14"),(lod,"v3","v15")):
        for i,flt in enumerate(vals):
            word=struct.unpack("<I",struct.pack("<f",flt))[0]
            if not i:k.lines.append(f"v_mov_b32 {reg}, 0x{word:08x}")
            else:k.lines.extend([f"v_mov_b32 v11, 0x{word:08x}",f"v_cmp_eq_u32 vcc_lo, {i}, {index}",
                                 f"v_cndmask_b32 {reg}, {reg}, v11, vcc_lo"])
    k.lines+=["s_load_dwordx8 s[16:23], s[6:7], 64","s_load_dwordx4 s[8:11], s[6:7], 96","s_waitcnt lgkmcnt(0)",
              "image_gather4_l v[4:7], v[1:3], s[16:23], s[8:11] dmask:1 dim:SQ_RSRC_IMG_2D","s_waitcnt vmcnt(0)"]
    # Actual texture contents, mirror in U and border in V, nonzero MIP bias
    # and clamp. Exercise negative coordinates, NPOT extents and a half-LOD tie.
    mn,mx,bias,border=.25,2.3,.5,-7.
    for tap,(dx,dy) in enumerate(((0,1),(1,1),(1,0),(0,0))):
        def oracle(i):
            j=i%16;m=math.floor(min(mx,max(mn,lod[j//4]+bias))+.5);ww,hh=max(1,w>>m),max(1,h>>m)
            x=(math.floor(u[j%4]*ww-.5)+dx)%(2*ww);x=min(x,2*ww-1-x)
            y=math.floor(v[j%4]*hh-.5)+dy
            value=1000*m+100*y+x if 0<=y<hh else border
            return struct.unpack("<I",struct.pack("<f",value))[0]
        k.store(f"v{4+tap}",oracle)
    k.save()
    stem=args.out/k.name
    data=bytes([1|(3<<3)])+struct.pack("<7f",mn,mx,bias,border,0.,0.,0.)
    views=[x for x, in struct.iter_unpack("<I",stem.with_suffix(".views").read_bytes())]
    values=bytes(data[(x&0x7fffffff)>>11] if x&0x80000000 else 0 for x in views)
    subprocess.run([args.compiler,"test-kernel",stem.with_suffix(".bin"),str(k.wave),str(k.threads),"0",
                    str(4*len(k.rows[0])),stem.with_suffix(".dxil"),"x"+values.hex()],check=True)
    with stem.with_suffix(".gather").open("wb") as f:
        f.write(struct.pack("<3I",w,h,levels))
        for m in range(levels):
            for y in range(max(1,h>>m)):
                for x in range(max(1,w>>m)):f.write(struct.pack("<f",1000*m+100*y+x))


if args.subset_bits:
    for near in (False,True):subset_bits(near)
elif args.queue_pc:
    for w in (32,64):queue_pc(w)
elif args.mask_words:mask_words()
elif args.barrier_loops:
    for w in (32,64):
        for dependent in (False,True):barrier_loop(w,dependent)
elif args.half_lds:
    for w in (32,64):
        for odd in (False,True):half_lds(w,odd)
elif args.compare_literals:
    compare_literals()
elif args.buffer_producer:
    for mutable in (False,True):buffer_producer(mutable)
    for poll in (False,True):counted_output(poll)
elif args.half_pairs:
    half_pairs()
elif args.vm_order:
    vm_order()
elif args.legacy_product:
    legacy_product()
elif args.group_waves:
    for wave in (32,64):
        for dynamic in (False,True):group_waves(wave,dynamic)
        nested_loops(wave)
elif args.lane_spill:
    lane_spill()
elif args.large_cfg:
    large_cfg()
elif args.nested_exec:
    nested_exec()
    large_cfg()
elif args.gather_lod:
    gather_lod()
elif args.image64:
    image64()
else:
    for typ in ("f16", "f32", "f64", "u16", "i16", "u32", "i32", "u64", "i64"):
        comparison(typ)
    for w in (32, 64):
        shared(w)
    packed()
    binary64()
    subword_memory()
    prefix_mask()
    scalar_literal()
    vcc_words()
    for w in (32,64):mask_edge(w)
    nested_exec()
print("Wrote ISA conformance suite:",args.out)
