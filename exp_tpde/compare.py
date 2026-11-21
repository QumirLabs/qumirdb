#!/usr/bin/env python3
"""Classifies TPDE test_reg results against the LLVM run.

usage: compare.py <llvm results.json> <tpde results.json>
"""
import collections
import json
import re
import sys

REASONS = [
    (re.compile(r"tpde: unsupported ([^\n\"@]*)"), "unsupported"),
    (re.compile(r"tpde: cannot compile ([^\n\"@]*)"), "cannot compile"),
    (re.compile(r"tpde codegen error: ([^\n\"@]*)"), "codegen error"),
    (re.compile(r"tpde: ([^\n\"@]*)"), "tpde"),
    (re.compile(r"codegen backend: ([^\n\"@]*)"), "backend"),
]


def reason(msg):
    for rx, kind in REASONS:
        m = rx.search(msg or "")
        if m:
            text = m.group(1).strip().rstrip("\\").strip()
            return kind, text
    return None


def main():
    llvm = json.load(open(sys.argv[1]))
    tpde = json.load(open(sys.argv[2]))
    total = len(tpde)
    stat = collections.Counter()
    kinds = collections.Counter()
    detail = collections.Counter()
    diverged = []
    for name, r in sorted(tpde.items()):
        base = llvm.get(name, {}).get("status")
        if r["status"] == "ok":
            stat["ok"] += 1
            if base != "ok":
                stat["ok (llvm fails)"] += 1
            continue
        why = reason(r.get("msg"))
        if why:
            stat["tpde error"] += 1
            kinds[why[0]] += 1
            detail[f"{why[0]}: {why[1]}"] += 1
            continue
        if base != "ok":
            stat["fails on llvm too"] += 1
            continue
        stat["diverged"] += 1
        diverged.append((name, r["status"]))
    print(f"total {total}")
    for k, v in stat.most_common():
        print(f"  {k}: {v}")
    print("tpde error kinds:")
    for k, v in kinds.most_common():
        print(f"  {k}: {v}")
    print("tpde error details:")
    for k, v in detail.most_common():
        print(f"  {v:5d}  {k}")
    print("diverged:")
    for name, status in diverged:
        print(f"  {status:6s} {name}")


if __name__ == "__main__":
    main()
