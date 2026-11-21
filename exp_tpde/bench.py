#!/usr/bin/env python3
"""Median kernel JIT times of qdb --timing: llvm (no cache), llvm (warm cache), tpde.

usage: bench.py <qdb> <data dir> <cache dir> <runs> <query.sql>...
"""
import os
import re
import statistics
import subprocess
import sys

qdb, data, cache, runs = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
queries = sys.argv[5:]
MODES = {
    "llvm": ["--kernel:backend=llvm"],
    "llvm-cache": ["--kernel:backend=llvm", f"--jit:cache_dir={cache}"],
    "tpde": ["--kernel:backend=tpde"],
}
STAGE = re.compile(r"\[kernel-timing\] (.*)")
KV = re.compile(r"([\w+-]+)=([\d.]+)ms")


def run(query, args):
    env = dict(os.environ, QUMIR_KERNEL_TIMING="1")
    out = subprocess.run([qdb, "--sql", "--data", data, "--timing", "--format", "null", "-i", query] + args,
                         capture_output=True, text=True, env=env)
    text = out.stdout + out.stderr
    jit = re.search(r"Jit(?:LLVM|Tpde): ([\d.]+) seconds", text)
    build = re.search(r"KernelBuild: ([\d.]+) seconds", text)
    stages = {}
    for line in STAGE.findall(text):
        for k, v in KV.findall(line):
            stages[k] = stages.get(k, 0.0) + float(v)
    if not jit:
        return None, text[-300:]
    return {"jit": float(jit.group(1)) * 1000, "build": float(build.group(1)) * 1000, **stages}, None


for q in queries:
    name = os.path.basename(q)
    for mode, args in MODES.items():
        if mode == "llvm-cache":
            run(q, args)  # warm the cache
        samples = []
        err = None
        for _ in range(runs):
            s, err = run(q, args)
            if s is None:
                break
            samples.append(s)
        if not samples:
            print(f"{name:12s} {mode:10s} error: {err!r}")
            continue
        keys = sorted({k for s in samples for k in s})
        med = {k: statistics.median(s.get(k, 0.0) for s in samples) for k in keys}
        parts = " ".join(f"{k}={med[k]:.1f}" for k in keys if k not in ("jit", "build"))
        print(f"{name:12s} {mode:10s} jit={med['jit']:.1f}ms kernelbuild={med['build']:.1f}ms | {parts}", flush=True)
