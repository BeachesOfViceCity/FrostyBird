#!/usr/bin/env python3
"""Resolve Frosty's UE shipping RVAs using the symbols supplied with the title."""
import argparse
from bisect import bisect_right
from pathlib import Path
import re
import struct
import zlib


def load(path):
    src = path.read_bytes()
    size, compressed = struct.unpack_from("<II", src)
    if compressed != len(src) - 8 or size > 256 << 20:
        raise ValueError("unexpected symbol file header")
    decoder = zlib.decompressobj()
    data = decoder.decompress(src[8:], size+1)
    if len(data) != size or not decoder.eof or decoder.unused_data:
        raise ValueError("truncated symbol data")
    version, name_len = struct.unpack_from("<II", data)
    if version != 1:
        raise ValueError("unsupported symbol format")
    pos = 8 + name_len + 8
    count, = struct.unpack_from("<I", data, pos)
    pos += 4
    names = pos + 12 * count
    if struct.unpack_from("<I", data, names)[0] != count:
        raise ValueError("symbol/name counts differ")
    names += 4
    result = []
    for _ in range(count):
        rva, length, _ = struct.unpack_from("<III", data, pos)
        pos += 12
        n, = struct.unpack_from("<I", data, names)
        if not n or names + 4 + n > len(data):
            raise ValueError("bad symbol name")
        result.append((rva, length, data[names+4:names+3+n].decode(errors="replace")))
        names += 4+n
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("symbols", type=Path)
    p.add_argument("rva", nargs="*", type=lambda v: int(v, 0))
    p.add_argument("--match", help="function name regexp (case-insensitive)")
    p.add_argument("--wine-log", type=Path, help="symbolize a UE crash stack using Wine's module-relative SEH trace")
    args = p.parse_args()
    rows = load(args.symbols)
    if args.match:
        pattern = re.compile(args.match, re.I)
        for addr, length, name in rows:
            if pattern.search(name):
                print(f"0x{addr:08x} ({length:#x}) {name}")
    if args.wine_log:
        text = args.wine_log.read_text(errors="replace")
        loaded = re.search(r'Loaded L"[^"]*Frosty-XSX-Shipping\.exe" at ([0-9a-f]+)', text, re.I)
        m = re.search(r"addr=([a-fA-F0-9]+) \(Frosty-XSX-Shipping\.exe\+([a-fA-F0-9]+)\)", text)
        if loaded:
            base=int(loaded[1],16)
        elif m:
            base=int(m[1],16)-int(m[2],16)
        else:
            p.error("no title image base in log; use WINEDEBUG=+loaddll,+seh")
        args.rva += [int(a, 16) - base for a in re.findall(r"^0x([0-9a-fA-F]+) UnknownFunction", text, re.M)]
    for addr in args.rva:
        i = bisect_right(rows, addr, key=lambda r: r[0]) - 1
        if i >= 0:
            start, n, s = rows[i]
            if addr - start < n:
                print(f"0x{addr:08x} {s}+0x{addr-start:x}")


if __name__ == "__main__":
    main()
