#!/usr/bin/env python3
"""Check the retail static-mesh MDF strip flag, using its adjacent serialized records."""
import argparse
import csv
from functools import lru_cache
import math
from pathlib import Path
import re
import struct
import sys
import zlib


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0]


class IoStore:
    """Single-partition V3 IoStore, reading just the blocks occupied by a requested span."""
    def __init__(self, path):
        b = self.toc = path.read_bytes()
        if b[:17] != b"-==--==--==--==-\x03" or u32(b, 32) != 12 or u32(b, 52) != 1:
            raise ValueError("unsupported IoStore header")
        self.data = path.with_suffix(".ucas").open("rb")
        entries = u32(b, 24)
        self.lengths = u32(b, 20) + 12 * entries
        self.blocks = self.lengths + 10 * entries
        self.block_size = u32(b, 44)
        end = self.blocks + 12 * u32(b, 28)
        n, w = struct.unpack_from("<II", b, 36)
        self.methods = [b"None"] + [b[end+j*w:end+(j+1)*w].rstrip(b"\0") for j in range(n)]
        end += n*w
        end += 4 + u32(b, end)  # directory index mount point
        self.dirs = end + 4
        end = self.dirs + 16*u32(b, end)
        self.files = end + 4
        end = self.files + 12*u32(b, end)
        self.strings = []
        count = u32(b, end)
        end += 4
        for _ in range(count):
            n, = struct.unpack_from("<i", b, end)
            end += 4
            size = n if n >= 0 else -2*n
            s = b[end:end+size].decode("utf-8" if n >= 0 else "utf-16-le").rstrip("\0")
            self.strings.append(s)
            end += size

    def files_in(self, i=0, prefix=""):
        b = self.toc
        name, sub, _, file = struct.unpack_from("<4I", b, self.dirs+16*i)
        if name != 0xffffffff:
            prefix += self.strings[name] + "/"
        while file != 0xffffffff:
            name, file, index = struct.unpack_from("<3I", b, self.files+12*file)
            yield prefix+self.strings[name], index
        while sub != 0xffffffff:
            yield from self.files_in(sub, prefix)
            sub = u32(b, self.dirs+16*sub+8)

    @lru_cache(maxsize=32)
    def block(self, i):
        at = self.blocks + 12*i
        word = self.toc[at:at+12]
        self.data.seek(int.from_bytes(word[:5], "little"))
        b = self.data.read(int.from_bytes(word[5:8], "little"))
        method = self.methods[word[11]]
        if method == b"Zlib":
            b = zlib.decompress(b)
        elif method != b"None":
            raise ValueError("unsupported compression: "+repr(method))
        if len(b) != int.from_bytes(word[8:11], "little"):
            raise ValueError("incorrect block length")
        return b

    def read(self, index, pos, size):
        p = self.lengths+index*10
        offset = int.from_bytes(self.toc[p:p+5], "big")
        total = int.from_bytes(self.toc[p+5:p+10], "big")
        if pos < 0 or size < 0 or pos+size > total:
            raise ValueError("export span exceeds chunk")
        data = bytearray()
        while len(data) < size:
            block, skip = divmod(offset+pos+len(data), self.block_size)
            part = self.block(block)[skip:skip+size-len(data)]
            if not part:
                raise ValueError("short compression block")
            data.extend(part)
        return data


def static_mesh_class(path):
    # The global ScriptObjects chunk supplies the class hash; package-local
    # export names and SM_ file naming conventions do not identify the class.
    b = path.read_bytes()
    n, size = struct.unpack_from("<2I", b)
    headers = 16 + n*8
    text = headers + n*2
    indices = set()
    for j in range(n):
        v = int.from_bytes(b[headers+2*j:headers+2*j+2], "big")
        width = 1 + (v >> 15)
        end = text + (v & 32767)*width
        if b[text:end] == "StaticMesh".encode("utf-8" if width == 1 else "utf-16-le"):
            indices.add(j)
        text = end
    if text != headers+n*2+size:
        raise ValueError("invalid global name batch")
    keys = {key for j, _, key, _, _ in struct.iter_unpack("<IIQQQ", b[text+4:text+4+32*u32(b, text)])
            if (j & 0x3fffffff) in indices}
    if len(keys) != 1:
        raise ValueError("ambiguous script class")
    return keys.pop()


def meshes(store, chunk, cls):
    h = struct.unpack("<10I", store.read(chunk, 0, 40))
    tab = store.read(chunk, h[6], h[8]-h[6])
    exports_end = h[7]-h[6]
    if exports_end % 72:
        raise ValueError("invalid export table")
    pos = h[0]
    for idx, cmd in struct.iter_unpack("<II", tab[exports_end:]):
        if cmd == 1:
            size = struct.unpack_from("<Q", tab, idx*72+8)[0]
            key = struct.unpack_from("<Q", tab, idx*72+32)[0]
            if key == cls:
                yield idx, pos, size
            pos += size


def cards_before(b, end):
    # Last populated LOD: FBox (six floats + validity), build LOD, count,
    # N serialized card OBBs (five vectors, level and direction bytes).
    for empty_lods in range(8):
        at = end - 4*empty_lods
        if at < 41 or any(b[at:end]):
            break
        for count in range(1, (at-37)//62+1):
            p = at-62*count
            vals = struct.unpack_from("<15fBB", b, p)
            axes = [vals[k:k+3] for k in (0, 3, 6)]
            if (not all(map(math.isfinite, vals[:15])) or vals[-1] >= 6 or min(vals[12:15]) < 0
                or any(abs(sum(x*x for x in axis)-1) > .02 for axis in axes)
                or any(abs(sum(x*y for x, y in zip(axes[j], axes[(j+1)%3]))) > .02 for j in range(3))):
                break
            if u32(b, p-4) != count or b[p-9] != 1 or u32(b, p-37) != 1:
                continue
            box = struct.unpack_from("<6f", b, p-33)
            if all(map(math.isfinite, box)) and all(box[j] <= box[j+3] for j in range(3)):
                return count
    return 0


screen_table = re.compile(rb"(?=(?:\x01\x00\x00\x00.{4}){8})", re.DOTALL)


def stripped(b):
    matches = []
    for m in screen_table.finditer(b):
        bounds = m.start()-32  # bounds, bool, eight cooked per-platform screen-size records
        if bounds < 43 or u32(b, bounds+28) > 1:
            continue
        global_flags, class_flags = b[bounds-2:bounds]
        if global_flags & ~3 or class_flags != 1:
            continue
        box = struct.unpack_from("<7f", b, bounds)
        if not all(map(math.isfinite, box)) or min(box[3:]) < 0:
            continue
        cards = cards_before(b, bounds-2)
        if cards:
            matches.append((bounds-2, global_flags, class_flags, cards))
    return matches[0] if len(matches) == 1 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("utoc", type=Path)
    ap.add_argument("--match", default=".", help="package path regular expression")
    args = ap.parse_args()
    pattern = re.compile(args.match, re.I)
    store = IoStore(args.utoc)
    cls = static_mesh_class(args.utoc.with_name("global.ucas"))
    out = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
    out.writerow(("package", "export", "status", "flag_offset_in_export", "global_flags", "class_flags", "adjacent_cards"))
    for path, i in store.files_in():
        if not path.endswith(".uasset") or not pattern.search(path):
            continue
        for idx, pos, size in meshes(store, i, cls):
            start = max(0, size-65536)
            result = stripped(store.read(i, pos+start, size-start))
            if result:
                off, *fields = result
                out.writerow((path, idx, "stripped", hex(start+off), *fields))
            else:
                out.writerow((path, idx, "unresolved", "", "", "", ""))


if __name__ == "__main__":
    main()
