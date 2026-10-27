#!/usr/bin/env python3
"""Freeze the existing TPC-H templates, then collect alternating A/B runs."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import signal
import statistics
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compare_results import compare_csv
from query_runner import TIMINGS, sha256


RUNNER = Path(__file__).with_name("run_tpch.sh")
REPO = RUNNER.parents[2]
FORBIDDEN = {"--format", "--verbose", "--scheduler-counters", "--operator-diagnostics",
             "--operator-instrumentation", "--instrumentation", "--sql", "--sexpr", "--data", "-i"}
SUM_METRICS = [*TIMINGS, "process_wall_seconds", "process_user_seconds", "process_system_seconds"]


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def git(*arguments):
    result = subprocess.run(["git", *arguments], cwd=REPO, text=True, capture_output=True)
    return result.stdout.strip() if result.returncode == 0 else None


def execute(command, directory, env, timeout):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "runner.stdout").open("wb") as stdout, (directory / "runner.stderr").open("wb") as stderr:
        process = subprocess.Popen(command, env=env, stdout=stdout, stderr=stderr, start_new_session=True)
        try:
            returncode = process.wait(timeout=timeout)
        except BaseException:
            # The subprocess group includes the shell, query collector and qdb.
            # Reap it on both a per-run timeout and an outer-stage interruption.
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            raise
    if returncode:
        raise RuntimeError(f"Runner exited {returncode}; see {directory / 'runner.stderr'}")


def file_state(files):
    return {str(path): (path.stat().st_size, path.stat().st_mtime_ns, path.stat().st_ino) for path in files}


def dataset_manifest(directory):
    files = sorted(directory.glob("*.parquet"))
    if not files:
        raise ValueError(f"No Parquet files in {directory}")
    entries = [{"name": path.name, "size_bytes": path.stat().st_size, "sha256": sha256(path)} for path in files]
    identity = hashlib.sha256(json.dumps(entries, sort_keys=True).encode()).hexdigest()
    return files, {"directory": str(directory), "files": entries, "dataset_manifest_sha256": identity}


def binary_manifest(path, declared_commit, arguments):
    path = Path(path).resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f"Not an executable: {path}")
    commit = git("rev-parse", declared_commit + "^{commit}")
    if commit is None:
        raise ValueError(f"Source commit is not available locally: {declared_commit}")
    for argument in arguments:
        if argument.split("=", 1)[0] in FORBIDDEN:
            raise ValueError(f"{argument}: SQL/data/output are shared; diagnostic counters require a separate run")
    version = subprocess.run([str(path), "--version"], text=True, capture_output=True, timeout=30)
    if version.returncode:
        raise ValueError(f"Could not obtain version of {path}")
    cache = path.parent.parent / "CMakeCache.txt"
    build = {}
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "LLVM_DIR:", "QDB_LLVM_LINK_SHARED:", "qumirdb_SOURCE_DIR:")):
                key, value = line.split("=", 1)
                build[key.split(":", 1)[0]] = value
        build["cmake_cache_sha256"] = sha256(cache)
    compiler = build.get("CMAKE_CXX_COMPILER")
    if compiler:
        compiler_version = subprocess.run([compiler, "--version"], text=True, capture_output=True, timeout=30)
        build["compiler_version"] = compiler_version.stdout.strip()
    build["declared_source_submodules"] = git("ls-tree", commit, "externals/coroio", "externals/qumir")
    return {"path": str(path), "binary_sha256": sha256(path), "declared_source_commit": commit,
            "version": version.stdout.strip(), "engine_args": arguments, "build": build}


def freeze_queries(config, out, env):
    rendered = out / "rendered"
    render_env = {**env, "MODE": "sql", "OUT_DIR": str(rendered), "RENDER_ONLY": "1"}
    execute(["bash", str(RUNNER), str(config.data), str(config.scale)], rendered, render_env, config.timeout)
    queries = {}
    for query in range(1, 23):
        path = rendered / "queries" / f"q{query}_sf{config.scale}.sql"
        if not path.is_file() or re.search(r"__[A-Z][A-Z0-9_]*__", path.read_text()):
            raise ValueError(f"Missing or unresolved rendered SQL: {path}")
        queries[query] = {"path": str(path), "sha256": sha256(path)}
        path.chmod(0o444)
    return queries


def run_sample(config, manifest, arm, kind, index, queries, env):
    out = config.out / kind / f"{arm}-{index}"
    out.mkdir(parents=True)
    info = manifest["arms"]["a" if arm == "control" else arm]
    engine_args = [] if arm == "control" else info["engine_args"]
    cache_dir = config.out / "jit-cache" / arm
    cache_dir.mkdir(parents=True, exist_ok=True)
    arguments = ["--format", "<escape>csv", "--jit-cache-dir", str(cache_dir), *engine_args]
    args_file = out / "arguments.nul"
    args_file.write_bytes(b"".join(argument.encode() + b"\0" for argument in arguments))
    run_env = {**env, "MODE": "sql", "QDB_BIN": info["path"], "QDB_ARGS_FILE": str(args_file),
               "FROZEN_QUERY_DIR": str(config.out / "rendered/queries"), "OUT_DIR": str(out)}
    command = ["bash", str(RUNNER), str(config.data), str(config.scale), ",".join(map(str, queries))]
    write_json(out / "invocation.json", {"argv": command, "environment": {key: run_env[key] for key in
               ("MODE", "QDB_BIN", "QDB_ARGS_FILE", "FROZEN_QUERY_DIR", "OUT_DIR")}})
    started = time.perf_counter()
    execute(command, out, run_env, config.timeout)
    records = [json.loads(line) for line in (out / f"tpch_sql_sf{config.scale}.jsonl").read_text().splitlines()]
    if sorted(record["query"] for record in records) != sorted(queries):
        raise RuntimeError(f"Incomplete query set in {out}")
    for record in records:
        query = record["query"]
        if record["returncode"] or any(record[key] is None for key in SUM_METRICS):
            raise RuntimeError(f"Missing timings or query failure: {record['stderr_path']}")
        if record["sql_sha256"] != manifest["queries"][str(query)]["sha256"]:
            raise RuntimeError("Rendered SQL changed during the campaign")
        record.update(run_id=manifest["run_id"], arm=arm, run_index=index, kind=kind,
                      warmup=(kind == "warmup"), binary_sha256=info["binary_sha256"],
                      declared_source_commit=info["declared_source_commit"],
                      dataset_manifest_sha256=manifest["dataset"]["dataset_manifest_sha256"])
        with (config.out / "runs.jsonl").open("a") as runs:
            runs.write(json.dumps(record, allow_nan=False) + "\n")
    manifest["samples"].append({"arm": arm, "kind": kind, "run_index": index,
                                 "suite_wall_seconds": time.perf_counter() - started, "directory": str(out)})
    return records


def compare_sample(config, manifest, records, reference):
    for record in records:
        query = record["query"]
        if query not in reference:
            reference[query] = record["stdout_path"]
            continue
        sql = Path(manifest["queries"][str(query)]["path"]).read_text()
        result = compare_csv(reference[query], record["stdout_path"], sql, config.rel_tol, config.abs_tol)
        result.update(query=query, arm=record["arm"], kind=record["kind"], run_index=record["run_index"],
                      reference=reference[query], candidate=record["stdout_path"])
        manifest["comparisons"].append(result)
        if result["status"] != "equal":
            raise RuntimeError(f"Result comparison {result['status']} for Q{query}: {result}")


def distribution(values):
    return {"runs": values, "median": statistics.median(values), "min": min(values), "max": max(values)}


def summarize(out):
    records = [json.loads(line) for line in (out / "runs.jsonl").read_text().splitlines()]
    summary = {"aggregation": "sum each complete measured run, then median across runs; RSS uses per-run peak", "arms": {}}
    for arm in ("a", "b"):
        selected = [record for record in records if record["arm"] == arm and record["kind"] == "measured"]
        indices = sorted({record["run_index"] for record in selected})
        totals = {key: distribution([sum(record[key] for record in selected if record["run_index"] == i) for i in indices]) for key in SUM_METRICS}
        totals["max_rss_bytes"] = distribution([max(record["max_rss_bytes"] for record in selected if record["run_index"] == i) for i in indices])
        per_query = {}
        for query in sorted({record["query"] for record in selected}):
            group = [record for record in selected if record["query"] == query]
            per_query[query] = {key: distribution([record[key] for record in group]) for key in [*SUM_METRICS, "max_rss_bytes"]}
        summary["arms"][arm] = {"totals": totals, "queries": per_query}
    return summary


def run_campaign(config):
    diagnostic_env = {"QDB_DEBUG_ROW_GROUP_PREDICATE", "QDB_DUMP_PASSES", "QDB_JIT_CACHE_STATS"}
    if diagnostic_env.intersection(os.environ):
        raise ValueError("Diagnostic environment variables require a separate run: " +
                         ", ".join(sorted(diagnostic_env.intersection(os.environ))))
    config.data = config.data.resolve()
    config.out = config.out.resolve()
    if config.out.exists() and any(config.out.iterdir()):
        raise ValueError("Output directory must be new or empty; previous samples are never overwritten")
    config.out.mkdir(parents=True, exist_ok=True)
    env = {key: value for key, value in os.environ.items() if key not in
           {"QDB_BIN", "QDB_ARGS", "QDB_ARGS_FILE", "RENDER_ONLY", "FROZEN_QUERY_DIR", "OUT_DIR", "MODE"}}
    arms = {"a": binary_manifest(config.a, config.a_commit, shlex.split(config.a_args)),
            "b": binary_manifest(config.b or config.a, config.b_commit or config.a_commit, shlex.split(config.b_args))}
    files, dataset = dataset_manifest(config.data / f"pq{config.scale}")
    states = file_state(files)
    binaries = {arm: sha256(info["path"]) for arm, info in arms.items()}
    manifest = {"schema_version": 1, "run_id": uuid.uuid4().hex, "status": "running", "scale": config.scale,
                "arms": arms, "dataset": dataset, "samples": [], "comparisons": [],
                "host": {"platform": platform.platform(), "machine": platform.machine(), "logical_cpus": os.cpu_count(),
                         "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
                         "python": sys.version, "environment": {key: env.get(key) for key in
                         ("JITDUMPDIR", "QUMIR_RUNTIME", "QDB_ARROW_MEMORY_POOL", "LD_PRELOAD", "OMP_NUM_THREADS", "ARROW_NUM_THREADS", "LC_ALL", "TZ")}},
                "harness": {"commit": git("rev-parse", "HEAD"), "tracked_changes": git("status", "--porcelain", "--untracked-files=no")},
                "cache_policy": "Dataset content hashing reads files before warmup; no page-cache eviction. Separate newly created JIT caches per arm, retained after warmup. Query-specific compilation may remain.",
                "comparison": {"format": "quoted CSV", "relative_tolerance": config.rel_tol, "absolute_tolerance": config.abs_tol,
                               "numeric_policy": "canonical floating spellings inferred by column; integer-only and other text columns exact", "row_order": "multiset preserving duplicates", "limited_mismatch": "requires review, never automatically accepted"},
                "cli_defaults": {"scheduler": "threaded", "scheduler_workers": os.cpu_count(), "scan_tasks": "auto"},
                "default_control": "skipped by explicit --skip-control" if config.skip_control else "all22queries, no engine tuning arguments; shared CSV/timing/input options remain"}
    manifest_path = config.out / "manifest.json"
    write_json(manifest_path, manifest)
    reference = {}
    try:
        queries = freeze_queries(config, config.out, env)
        manifest["queries"] = queries
        # JSON keys and the on-disk manifest use the same representation.
        manifest["queries"] = {str(key): value for key, value in queries.items()}
        schedule = [] if config.skip_control else [("control", "control", 0, list(queries))]
        for i in range(config.warmups):
            schedule.extend((arm, "warmup", i + 1, config.queries) for arm in ("a", "b"))
        for i in range(config.runs):
            schedule.extend((arm, "measured", i + 1, config.queries) for arm in ("a", "b"))
        for arm, kind, index, selected in schedule:
            if file_state(files) != states or any(sha256(info["path"]) != binaries[name] for name, info in arms.items()):
                raise RuntimeError("Dataset or binary changed during campaign")
            records = run_sample(config, manifest, arm, kind, index, selected, env)
            if file_state(files) != states or any(sha256(info["path"]) != binaries[name] for name, info in arms.items()):
                raise RuntimeError("Dataset or binary changed during a sample")
            compare_sample(config, manifest, records, reference)
            write_json(manifest_path, manifest)
            print(f"{kind} {arm} {index}: {len(records)} queries collected; result gates passed", flush=True)
        if dataset_manifest(config.data / f"pq{config.scale}")[1] != dataset:
            raise RuntimeError("Dataset content changed during campaign")
        write_json(config.out / "summary.json", summarize(config.out))
        manifest["status"] = "complete"
    except BaseException as error:
        manifest.update(status="failed", error=str(error))
        raise
    finally:
        write_json(manifest_path, manifest)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=r"""Example (Q19 runtime filters; repeat with --scale 10 in a new output directory):
  python3 benchmark/tpch/compare_runs.py --data /data/tpch --scale 1 --queries 19 \
    --a build/bin/qdb --a-commit <build-source-sha> \
    --a-args='--scheduler threaded --scheduler-workers 2' \
    --b-args='--scheduler threaded --scheduler-workers 2 --no-runtime-filters' \
    --out build/results/q19-sf1

The runner first saves all 22 SQL files using the existing templates/scale params.
A default-settings full-suite control precedes warmup A,B and measured A,B,A,B,A,B.
CSV/timing/input flags are shared; no engine tuning arguments enter the control.
Dataset hashing warms file pages; no cache eviction occurs. Each arm starts with
an empty persistent JIT cache, then retains it. Query compilation is reported
separately. Provenance declares the build source; binary version text is not proof.
Raw stdout/stderr, commands, frozen SQL, per-query JSONL and medians are retained.
RSS is bytes (per-query peak; suite peak is max, not a sum). All other totals sum
within each measured run before taking the median. Floating-looking numeric
columns use tolerance; CSV has no types, so numeric-looking text is ambiguous.
Integer-only/text columns compare exactly, row order is ignored, duplicates count.
TOP/LIMIT differences fail for manual boundary-tie review; they are never waived.
Profile/diagnostic runs must be separate. This tool does not establish that a
small-scale campaign satisfies the SF100/18-thread acceptance criteria of #77.
""")
    parser.add_argument("--data", type=Path, required=True, help="Base directory containing pq1/pq10/pq100")
    parser.add_argument("--scale", type=int, required=True)
    parser.add_argument("--queries", default=",".join(map(str, range(1, 23))))
    parser.add_argument("--a", type=Path, required=True, help="A executable")
    parser.add_argument("--b", type=Path, help="B executable (default: A)")
    parser.add_argument("--a-commit", required=True, help="Declared build source commit; version text alone does not establish provenance")
    parser.add_argument("--b-commit", help="Declared B build source commit (default: A)")
    parser.add_argument("--a-args", default="", help="Shell-quoted engine arguments; no shell is executed")
    parser.add_argument("--b-args", default="")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=1800, help="Seconds per complete runner invocation")
    parser.add_argument("--skip-control", action="store_true", help="Skip the full default-settings control; record this limitation")
    parser.add_argument("--rel-tol", type=float, default=1e-9)
    parser.add_argument("--abs-tol", type=float, default=1e-6)
    config = parser.parse_args()
    try:
        config.queries = list(map(int, config.queries.split(",")))
    except ValueError:
        parser.error("Queries must be comma-separated integers")
    if config.runs < 3 or config.warmups < 1 or not math.isfinite(config.timeout) or config.timeout <= 0:
        parser.error("At least 3 measured runs, 1 warmup and a positive timeout are required")
    if not config.queries or len(set(config.queries)) != len(config.queries) or any(q < 1 or q > 22 for q in config.queries):
        parser.error("Queries must be distinct numbers from 1 to 22")
    if any(not math.isfinite(value) or value < 0 for value in (config.rel_tol, config.abs_tol)):
        parser.error("Tolerances must be finite and nonnegative")
    def interrupted(number, frame):
        raise SystemExit(128 + number)
    signal.signal(signal.SIGTERM, interrupted)
    run_campaign(config)


if __name__ == "__main__":
    main()
