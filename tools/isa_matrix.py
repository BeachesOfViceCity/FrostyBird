#!/usr/bin/env python3
"""Join the catalog probes, retail audit, and passing GPU cases without conflating them."""
import argparse
import csv
from pathlib import Path
import re


def rows(path, key):
    with path.open() as f:
        data = list(csv.DictReader(f, delimiter="\t"))
    out = {r[key]: r for r in data}
    if not data or len(out) != len(data):
        raise ValueError(f"empty or duplicate entries in {path}")
    return out


p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--audit", required=True, type=Path)
p.add_argument("--catalog", required=True, type=Path)
p.add_argument("--suite", required=True, type=Path)
p.add_argument("--gpu-log", required=True, action="append", type=Path)
p.add_argument("--out", required=True, type=Path)
a = p.parse_args()
catalog = rows(a.catalog, "official_mnemonic")
archive = rows(a.audit / "opcodes.tsv", "opcode")
if not catalog.keys() <= archive.keys() or not (a.audit/"summary.txt").is_file():
    p.error("need a completed audit and its corresponding catalog")
passed = None
for log in a.gpu_log:
    text = log.read_text(errors="replace")
    end = re.search(r"^ISA conformance: (\d+) kernels, 0 incorrect words\s*$", text, re.M)
    cases = {name for name, good, total in re.findall(r"^([\w-]+): (\d+)/(\d+) words\s*$", text, re.M)
             if good == total and int(total) > 0}
    if not end or len(cases) != int(end[1]):
        p.error(f"incomplete or failed conformance run: {log}")
    passed = cases if passed is None else passed & cases
witness = {}
for case in sorted(passed):
    for line in (a.suite/(case+".s")).read_text().splitlines():
        if not line.strip():
            continue
        name = re.sub(r"_(?:e32|e64|dpp8?|sdwa)$", "", line.split()[0])
        witness.setdefault(name, set()).add(case)
a.out.parent.mkdir(parents=True, exist_ok=True)
with a.out.open("w", newline="") as f:
    out = csv.writer(f, delimiter="\t", lineterminator="\n")
    out.writerow(("mnemonic", "family", "base_decode", "isolated_compute_probe", "retail_records",
                  "in_default_dxil_records", "retail_example", "in_passing_gpu_cases"))
    for op, r in sorted(archive.items()):
        c = catalog.get(op, {})
        out.writerow((op, r["family"], c.get("status", "Xbox extension"),
                      c.get("isolated_lowering", "no vendor specimen"), r["records"],
                      r["in_dxil_records"], r["example"], ",".join(sorted(witness.get(op, []))) or "-"))
print(f"Wrote {a.out}: {len(catalog)} official mnemonics, {len(archive)-len(catalog)} additional spellings.")
