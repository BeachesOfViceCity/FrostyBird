#!/usr/bin/env python3
"""Relift exact prebound raygens after a compiler change, preserving the checked plans."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from frosty import BUILD, play_mount
from shader_cache import atomic_write


def refresh(tool, pak, plans, index, jobs, publish, report):
    contract = tuple(map(int, (plans / "dispatch.contract").read_text().split()))
    with index.open() as f:
        records = {r["collection_key"]: r for r in csv.DictReader(f, delimiter="\t")
                   if r["frequency"] == "6" and r["detail"] == "ok"}
    def translate(plan):
        k = plan.name.removesuffix(".checked.plan")
        record = records[k]
        rows = plan.read_text().splitlines()
        words, route, field, value, tlas, count = map(int, rows[0].split())
        if (words, route, field, value) != contract or count != len(rows) - 1:
            raise ValueError(f"{k}: checked contract does not match")
        args = [pak, record["library"], record["index"]]
        abi = (plans / (k + ".rt.abi")).read_bytes()
        if len(abi) != 8 or struct.unpack("<II", abi) != (words * 4, 8):
            raise ValueError(f"{k}: incompatible payload or attributes")
        layout = {}
        for row in rows[1:]:
            raw, dim, ty = row.split()
            r = int(raw)
            if r & ~0x17f or r == tlas or r in layout or int(dim) <= 0:
                raise ValueError(f"{k}: invalid global binding")
            layout[r] = ("u" if r & 256 else "t") + str(r & 127) + "=" + ty
        with tempfile.TemporaryDirectory(dir=BUILD) as tmp:
            folder = Path(tmp)
            subprocess.run([tool, "rt-lift-checked", *args, folder, str(words), ",".join(layout.values()),
                            str(route), str(field), str(value)], check=True, stdout=subprocess.DEVNULL)
            source = (folder / (k + ".xsrt")).read_bytes()
            if source != (plans / (k + ".xsrt")).read_bytes() or (folder / (k + ".rt.abi")).read_bytes() != abi:
                raise ValueError(f"{k}: different original collection or ABI")
            product = folder / (k + ".checked.dxil")
            target = plans / product.name
            old, new = target.read_bytes(), product.read_bytes()
            if new[:4] != b"DXBC" or struct.unpack_from("<I", new, 24)[0] != len(new):
                raise ValueError(f"{k}: invalid translated library")
            return target, new, dict(key=k, source_sha256=hashlib.sha256(source).hexdigest(),
                                     old_bytes=len(old), new_bytes=len(new), changed=old != new)
    with ThreadPoolExecutor(jobs) as pool:
        products = list(pool.map(translate, sorted(plans.glob("*.checked.plan"))))
    if not products:
        raise ValueError(f"no checked raygens in {plans}")
    if publish:
        for target, data, item in products:
            if item["changed"]:
                atomic_write(target, data)
    result = dict(contract=contract, published=publish, libraries=[item for _, _, item in products])
    if report:
        atomic_write(report, (json.dumps(result, indent=2) + "\n").encode())
    print(f"{len(products)} source/ABI matches, {sum(r['changed'] for r in result['libraries'])} changed checked raygens"
          f" ({'published' if publish else 'comparison only'})")


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--game")
    cli.add_argument("--tool", type=Path, default=BUILD / "host/frosty-shader")
    cli.add_argument("--plans", type=Path, default=BUILD / "street-shadow-demand")
    cli.add_argument("--index", type=Path, default=BUILD / "hwrt-package/ray-sha-index.tsv")
    cli.add_argument("--jobs", type=int, choices=range(1, 17), default=3)
    cli.add_argument("--publish", action="store_true")
    cli.add_argument("--report", type=Path)
    args = cli.parse_args()
    refresh(args.tool, play_mount(args.game) / "Frosty/Content/Paks/Frosty-XSX.pak",
            args.plans, args.index, args.jobs, args.publish, args.report)
