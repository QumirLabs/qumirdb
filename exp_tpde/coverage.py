#!/usr/bin/env python3
"""Per unique SQL case: which unsupported sets block TPDE, what a feature would unblock.

usage: coverage.py <llvm results.json> <tpde results.json>
"""
import collections
import itertools
import json
import re
import sys

llvm = json.load(open(sys.argv[1]))
tpde = json.load(open(sys.argv[2]))
RX = re.compile(r"tpde: unsupported ([^\n\"@]*)")


def base(name):
    return re.sub(r"_(single|threaded)(_vm)?$", "", name.removeprefix("RegQuery."))


cases = collections.defaultdict(list)
for name, r in tpde.items():
    if name.endswith("_plan"):
        continue
    cases[base(name)].append((name, r))
blocked = {}
status = collections.Counter()
for case, runs in cases.items():
    if all(llvm.get(n, {}).get("status") != "ok" for n, _ in runs):
        status["llvm fails"] += 1
        continue
    if all(r["status"] == "ok" for _, r in runs):
        status["ok"] += 1
        continue
    reasons = set()
    for _, r in runs:
        m = RX.search(r.get("msg") or "")
        if m:
            reasons |= {x.strip().rstrip("\\").strip() for x in m.group(1).split("|")}
    if reasons:
        status["unsupported"] += 1
        blocked[case] = frozenset(reasons)
    else:
        status["other"] += 1
print("unique cases:", len(cases), dict(status))
single = collections.Counter()
for s in blocked.values():
    for x in s:
        single[x] += 1
print("cases containing each construct:")
for x, n in single.most_common():
    alone = sum(1 for s in blocked.values() if s <= {x})
    print(f"  {n:4d} contain, {alone:4d} unblocked by it alone: {x}")
feats = sorted(single)
best = []
for k in (2, 3):
    for combo in itertools.combinations(feats, k):
        n = sum(1 for s in blocked.values() if s <= set(combo))
        best.append((n, combo))
best.sort(reverse=True)
print("best pairs/triples:")
for n, combo in best[:8]:
    print(f"  {n:4d} unblocked by {' + '.join(combo)}")
