#!/usr/bin/env python3
"""Group 'prof ADDR COUNT' lines from the SynthOS prof command by kernel function.

usage: tools/profsym.py build/serial/kernel.elf < output.txt
"""
import bisect
import re
import subprocess
import sys

syms = []
for line in subprocess.run(["nm", "-n", sys.argv[1]], capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[1] in "tTwW":
        syms.append((int(parts[0], 16), parts[2]))
addrs = [a for a, _ in syms]
totals, samples = {}, 0
for line in sys.stdin:
    m = re.search(r"prof ([0-9a-f]{8}) (\d+)", line)
    if not m:
        m2 = re.search(r"prof: (\d+) samples", line)
        if m2:
            samples = int(m2.group(1))
        continue
    addr, count = int(m.group(1), 16), int(m.group(2))
    i = bisect.bisect_right(addrs, addr) - 1
    name = syms[i][1] if i >= 0 and addr < 0x400000 else "(program code)"
    totals[name] = totals.get(name, 0) + count
for name, count in sorted(totals.items(), key=lambda kv: -kv[1]):
    print(f"{count:7d} {100.0 * count / max(samples, 1):5.1f}%  {name}")
