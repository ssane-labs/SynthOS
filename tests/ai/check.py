#!/usr/bin/env python3
"""Runs the `ai next` cases of tests/aimodel.py's tiny models through the engine built for the host (tests/ai/llm.c),
once on the plain C paths and once on this processor's SIMD paths. usage: check.py llmtest DIR"""
import subprocess
import sys

exe, d = sys.argv[1], sys.argv[2]
lines = open(d + "/cases.txt").read().split("\n")
model, fails, runs = None, 0, 0
for i in range(0, len(lines) - 1, 2):
    cmd, want = lines[i], lines[i + 1]
    if cmd.startswith("ai load "):
        model = d + "/" + cmd.split("/")[-1]
    elif cmd.startswith("ai next ") and model:
        args = cmd.split()[2:]
        for mode in (["--plain"], []):
            got = subprocess.run([exe, model] + mode + args, capture_output=True, text=True).stdout.strip()
            runs += 1
            if got != want:
                fails += 1
                print("FAIL %s %s: %s, want %s" % (model.split("/")[-1], " ".join(mode), got, want))
print("llm: %d runs, %d failed" % (runs, fails))
sys.exit(1 if fails else 0)
