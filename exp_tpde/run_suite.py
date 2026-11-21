#!/usr/bin/env python3
"""Runs test_reg RegQuery cases in N parallel chunks, survives crashes and hangs.

Each chunk reruns test_reg on its remaining tests after a crash or a stall,
recording the test that was running as crash/hang.
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time

RUN_RE = re.compile(r"^\[ RUN      \] (\S+)")
OK_RE = re.compile(r"^\[       OK \] (\S+) \((\d+) ms\)")
FAIL_RE = re.compile(r"^\[  FAILED  \] (\S+) \((\d+) ms\)")


def list_tests(binary, root):
    out = subprocess.run([binary, root, "--gtest_list_tests"], capture_output=True, text=True, check=True).stdout
    tests = []
    suite = None
    for line in out.splitlines():
        if not line.startswith(" "):
            suite = line.strip()
            continue
        name = line.strip().split()[0]
        if suite == "RegQuery.":
            tests.append(suite + name)
    return tests


def run_chunk(idx, tests, args, results, lock):
    remaining = list(tests)
    attempt = 0
    while remaining:
        attempt += 1
        log_path = os.path.join(args.out, f"chunk{idx}_run{attempt}.log")
        cmd = [args.bin, args.root, f"--kernel:backend={args.backend}"] + args.extra + [
            "--gtest_filter=" + ":".join(remaining)]
        with open(log_path, "w") as log:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    errors="replace", bufsize=1, start_new_session=True)
            current = None
            buf = []
            last = [time.monotonic()]
            done_here = set()
            killed = [False]

            def watchdog():
                while proc.poll() is None:
                    if time.monotonic() - last[0] > args.stall:
                        killed[0] = True
                        os.killpg(proc.pid, signal.SIGKILL)
                        return
                    time.sleep(1)

            threading.Thread(target=watchdog, daemon=True).start()
            for line in proc.stdout:
                last[0] = time.monotonic()
                log.write(line)
                line = line.rstrip("\n")
                m = RUN_RE.match(line)
                if m:
                    current = m.group(1)
                    buf = []
                    continue
                m = OK_RE.match(line)
                if m and m.group(1) == current:
                    with lock:
                        results[current] = {"status": "ok", "ms": int(m.group(2))}
                    done_here.add(current)
                    current = None
                    continue
                m = FAIL_RE.match(line)
                if m and m.group(1) == current:
                    with lock:
                        results[current] = {"status": "fail", "ms": int(m.group(2)), "msg": "\n".join(buf)[-20000:]}
                    done_here.add(current)
                    current = None
                    continue
                if current:
                    buf.append(line)
            rc = proc.wait()
        if current:
            status = "hang" if killed[0] else "crash"
            with lock:
                results[current] = {"status": status, "rc": rc, "msg": "\n".join(buf)[-20000:]}
            done_here.add(current)
        before = len(remaining)
        remaining = [t for t in remaining if t not in done_here]
        if len(remaining) == before:
            # No progress (setup failure): record the rest as errors.
            with lock:
                for t in remaining:
                    results[t] = {"status": "setup", "rc": rc}
            break
        print(f"chunk {idx}: attempt {attempt} rc={rc} left={len(remaining)}", flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin", required=True)
    p.add_argument("--root", required=True)
    p.add_argument("--backend", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--chunks", type=int, default=4)
    p.add_argument("--stall", type=int, default=300)
    p.add_argument("--filter", default="")
    p.add_argument("extra", nargs="*")
    args = p.parse_args()
    os.makedirs(args.out, exist_ok=True)
    tests = list_tests(args.bin, args.root)
    if args.filter:
        rx = re.compile(args.filter)
        tests = [t for t in tests if rx.search(t)]
    print(f"{len(tests)} tests", flush=True)
    chunks = [tests[i::args.chunks] for i in range(args.chunks)]
    results = {}
    lock = threading.Lock()
    started = time.monotonic()
    threads = [threading.Thread(target=run_chunk, args=(i, c, args, results, lock)) for i, c in enumerate(chunks)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump(results, f, indent=0)
    counts = {}
    for r in results.values():
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    print(f"done in {time.monotonic() - started:.0f}s: {counts}", flush=True)


if __name__ == "__main__":
    main()
