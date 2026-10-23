#!/usr/bin/env python3
"""Run one qdb process; keep processing, compilation and process costs separate."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


NUMBER = r"([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)"
TIMINGS = {
    "processing_seconds": rf"(?:Returned [0-9]+ rows in |Processed [0-9]+ rowsets in ){NUMBER}",
    "execution_cpu_seconds": rf"Cpu: {NUMBER} seconds",
    "planning_seconds": rf"Planning: {NUMBER} seconds",
    "kernel_build_seconds": rf"KernelBuild: {NUMBER} seconds",
    "jit_llvm_seconds": rf"JitLLVM: {NUMBER} seconds",
}


def parse_timings(stderr):
    result = {}
    for name, pattern in TIMINGS.items():
        matches = re.findall(pattern, stderr)
        result[name] = float(matches[-1]) if matches else None
    return result


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_query(binary, label, result_file, log_file, metrics_file, arguments):
    metrics_file = Path(metrics_file).resolve()
    query_dir = metrics_file.parent / "queries"
    query_dir.mkdir(parents=True, exist_ok=True)
    name = re.sub(r"[^A-Za-z0-9_.-]", "_", label)
    stdout_path = query_dir / (name + ".stdout")
    stderr_path = query_dir / (name + ".stderr")
    command = [str(Path(binary).resolve()), "--timing", *arguments]
    started = time.perf_counter()
    usage = None
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        except OSError as error:
            stderr.write(str(error).encode())
            returncode = 127
        else:
            _, status, usage = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
            returncode = process.returncode
    process_wall = time.perf_counter() - started
    if returncode < 0:
        returncode = 128 - returncode
    stderr_text = stderr_path.read_text(errors="replace")
    record = {
        "schema_version": 1,
        "label": label,
        "argv": command,
        "returncode": returncode,
        "process_wall_seconds": process_wall,
        "process_user_seconds": usage.ru_utime if usage else None,
        "process_system_seconds": usage.ru_stime if usage else None,
        "max_rss_bytes": int(usage.ru_maxrss) * (1 if sys.platform == "darwin" else 1024) if usage else None,
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
        **parse_timings(stderr_text),
    }
    identity = re.fullmatch(r"Q([0-9]+) sf([0-9]+)", label)
    if identity:
        record.update(query=int(identity[1]), scale=int(identity[2]))
    if "-i" in arguments:
        sql_path = Path(arguments[arguments.index("-i") + 1]).resolve()
        record.update(sql_path=str(sql_path), sql_sha256=sha256(sql_path))
    with Path(result_file).open("a") as results:
        results.write(f"\n=== {label} (rc={returncode}) ===\n")
        results.write(stdout_path.read_text(errors="replace"))
    with Path(log_file).open("a") as log:
        log.write(f"\n=== {label} (rc={returncode}) ===\n{stderr_text}\n")
    with metrics_file.open("a") as metrics:
        metrics.write(json.dumps(record, allow_nan=False) + "\n")
    return record


def main():
    if not hasattr(os, "wait4") or sys.platform not in ("linux", "darwin"):
        raise SystemExit("Per-process CPU/RSS collection requires Linux or macOS wait4")
    binary, label, results, log, metrics, *arguments = sys.argv[1:]
    record = run_query(binary, label, results, log, metrics, arguments)
    # Keep the existing shell runner's table and aggregate interface.
    fields = [record[name] or 0 for name in TIMINGS]
    print(*fields, record["returncode"])


if __name__ == "__main__":
    main()
