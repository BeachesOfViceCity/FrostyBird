#!/usr/bin/env python3
"""Sample read-only amdgpu board counters alongside a bounded validation process."""
import argparse
import csv
import os
from pathlib import Path
import select
import time


def sample(pid, output, device_id="0x73a5"):
    cards = [p / "device" for p in Path("/sys/class/drm").glob("card[0-9]*")
             if (p / "device/device").is_file() and (p / "device/device").read_text().strip() == device_id
             and (p / "device/vendor").read_text().strip() == "0x1002"]
    if len(cards) != 1:
        raise ValueError(f"expected one AMD board with PCI device ID {device_id}")
    card, = cards
    hw = next((card / "hwmon").glob("hwmon*"))
    paths = (card / "gpu_busy_percent", card / "mem_busy_percent", hw / "freq1_input",
             hw / "power1_average", card / "mem_info_vram_used", card / "mem_info_gtt_used")
    def counter(path):
        try:
            return int(path.read_text())
        except (OSError, ValueError):
            return ""
    with os.fdopen(os.pidfd_open(pid), "rb", buffering=0) as done, output.open("x") as f:
        rows = csv.writer(f, delimiter="\t", lineterminator="\n")
        rows.writerow(("unix_us", "gpu_busy_pct", "mem_busy_pct", "gfx_hz", "power_uw", "vram_bytes", "gtt_bytes"))
        while True:
            rows.writerow((time.time_ns() // 1000, *(counter(p) for p in paths)))
            if select.select([done], [], [], .25)[0]:
                break


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int, help="stop when this validation process exits")
    parser.add_argument("output", type=Path)
    parser.add_argument("--device-id", default="0x73a5")
    args = parser.parse_args()
    sample(args.pid, args.output, args.device_id)
