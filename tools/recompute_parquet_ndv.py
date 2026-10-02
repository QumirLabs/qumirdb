#!/usr/bin/env python3
"""Recompute capped NDVs and replace only the same-size Parquet stats footer value.

The scan phase never changes Parquet files. Apply uses a saved scan plan,
verifies each footer, backs it up, and replaces the custom `stats` JSON in place.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

import pyarrow.parquet as pq


def footer(path):
    size = path.stat().st_size
    with path.open("rb") as stream:
        stream.seek(size - 8)
        length, magic = struct.unpack("<I4s", stream.read(8))
        if magic != b"PAR1" or length > size - 8:
            raise ValueError(f"invalid Parquet footer: {path}")
        start = size - 8 - length
        stream.seek(start)
        data = stream.read(length)
    return size, start, data


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def stats_value(path):
    metadata = pq.ParquetFile(path).metadata
    values = metadata.metadata or {}
    if b"stats" not in values:
        return metadata, None
    return metadata, values[b"stats"]


def replacement(old_value, updates):
    stats = json.loads(old_value)
    for column in stats["columns"]:
        if column["name"] in updates:
            if column.get("ndv_exact") is not False:
                raise ValueError(f"{column['name']} is no longer inexact")
            column["ndv"] = updates[column["name"]]["new"]
    encoded = json.dumps(stats, separators=(",", ":"),
                         ensure_ascii=False).encode("utf-8")
    if len(encoded) > len(old_value):
        raise ValueError("updated stats do not fit original footer value")
    return encoded + b" " * (len(old_value) - len(encoded))


def scan_file(path, scanner):
    metadata, original = stats_value(path)
    if original is None:
        return None
    columns = [c for c in json.loads(original)["columns"]
               if c.get("ndv") is not None and c.get("ndv_exact") is False]
    if not columns:
        return None
    names = [c["name"] for c in columns]
    result = subprocess.run([str(scanner), str(path), *names],
                            text=True, capture_output=True, check=True)
    measured = {}
    for line in result.stdout.splitlines():
        name, distinct, non_null = line.split("\t")
        measured[name] = (int(distinct), int(non_null))
    if set(measured) != set(names):
        raise ValueError(f"scanner returned wrong columns: {path}")
    updates = {}
    for column in columns:
        name = column["name"]
        measured_ndv, non_null = measured[name]
        old_ndv = int(column["ndv"])
        updates[name] = {
            "old": old_ndv,
            "new": max(old_ndv, measured_ndv),
            "non_null_rows": non_null,
        }
    new_value = replacement(original, updates)  # check footer capacity
    size, _, data = footer(path)
    if data.count(original) != 1:
        raise ValueError(f"stats value is not unique in footer: {path}")
    new_data = data.replace(original, new_value, 1)
    return {
        "path": str(path.resolve()),
        "size": size,
        "rows": metadata.num_rows,
        "row_groups": metadata.num_row_groups,
        "footer_sha256": sha256(data),
        "new_footer_sha256": sha256(new_data),
        "stats_sha256": sha256(original),
        "updates": updates,
    }


def make_plan(roots, scanner, plan_path):
    entries = []
    for root in roots:
        for scale in (1, 10, 100):
            directory = root / f"pq{scale}"
            if not directory.is_dir():
                continue
            for path in sorted(directory.glob("*.parquet")):
                metadata, original = stats_value(path)
                if original is None or not any(
                    c.get("ndv_exact") is False
                    for c in json.loads(original)["columns"]
                ):
                    continue
                print(f"scan {path}", flush=True)
                entry = scan_file(path, scanner)
                if entry:
                    entries.append(entry)
                    changes = ", ".join(
                        f"{name}: {v['old']} -> {v['new']}"
                        for name, v in entry["updates"].items())
                    print(f"  {changes}", flush=True)
    plan = {"format": 2, "method": "HLL p=18", "entries": entries}
    plan_path.write_text(json.dumps(plan, indent=2) + "\n")
    print(f"plan: {plan_path}; files: {len(entries)}; "
          f"columns: {sum(len(e['updates']) for e in entries)}", flush=True)


def backup_path(backup_dir, path):
    return backup_dir / (sha256(str(path).encode())[:20] + ".footer")


def check_entry(entry, backup_dir):
    path = Path(entry["path"])
    size, start, data = footer(path)
    if size != entry["size"]:
        raise ValueError(f"file size changed since scan: {path}")
    backup = backup_path(backup_dir, path)
    if sha256(data) == entry["new_footer_sha256"]:
        if not backup.exists() or sha256(backup.read_bytes()) != entry["footer_sha256"]:
            raise ValueError(f"missing original footer backup: {path}")
        metadata, value = stats_value(path)
        if metadata.num_rows != entry["rows"] or metadata.num_row_groups != entry["row_groups"]:
            raise ValueError(f"row layout changed since scan: {path}")
        observed = {c["name"]: c for c in json.loads(value)["columns"]}
        for name, update in entry["updates"].items():
            if observed[name]["ndv"] != update["new"] or observed[name]["ndv_exact"] is not False:
                raise ValueError(f"updated NDV failed validation: {name}")
        return "updated"
    if sha256(data) != entry["footer_sha256"]:
        raise ValueError(f"footer changed since scan: {path}")
    metadata, original = stats_value(path)
    if original is None or sha256(original) != entry["stats_sha256"]:
        raise ValueError(f"stats changed since scan: {path}")
    if metadata.num_rows != entry["rows"] or metadata.num_row_groups != entry["row_groups"]:
        raise ValueError(f"row layout changed since scan: {path}")
    new_value = replacement(original, entry["updates"])
    offset = data.find(original)
    if offset < 0 or data.count(original) != 1:
        raise ValueError(f"stats value is not unique in footer: {path}")
    if sha256(data.replace(original, new_value, 1)) != entry["new_footer_sha256"]:
        raise ValueError(f"updated footer differs from scan plan: {path}")

    if backup.exists():
        if backup.read_bytes() != data:
            raise ValueError(f"conflicting backup: {backup}")
    return "pending"


def apply_entry(entry, backup_dir):
    if check_entry(entry, backup_dir) == "updated":
        print(f"already updated {entry['path']}", flush=True)
        return
    path = Path(entry["path"])
    size, start, data = footer(path)
    _, original = stats_value(path)
    new_value = replacement(original, entry["updates"])
    offset = data.find(original)
    backup = backup_path(backup_dir, path)
    if not backup.exists():
        with backup.open("xb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())

    try:
        with path.open("r+b") as stream:
            stream.seek(start + offset)
            stream.write(new_value)
            stream.flush()
            os.fsync(stream.fileno())
        updated, value = stats_value(path)
        if updated.num_rows != entry["rows"] or updated.num_row_groups != entry["row_groups"]:
            raise ValueError("row layout changed after write")
        observed = {c["name"]: c for c in json.loads(value)["columns"]}
        for name, update in entry["updates"].items():
            if observed[name]["ndv"] != update["new"] or observed[name]["ndv_exact"] is not False:
                raise ValueError(f"updated NDV failed validation: {name}")
        if path.stat().st_size != size:
            raise ValueError("file size changed after write")
        if sha256(footer(path)[2]) != entry["new_footer_sha256"]:
            raise ValueError("updated footer differs from scan plan")
    except Exception:
        with path.open("r+b") as stream:
            stream.seek(start)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        raise
    print(f"updated {path}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="*", type=Path,
                        help="benchmark roots containing pq1/pq10/pq100")
    parser.add_argument("--scanner", type=Path, default=Path("build/bin/qdb_ndv_scan"))
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--backup-dir", type=Path)
    args = parser.parse_args()
    if args.apply:
        plan = json.loads(args.plan.read_text())
        if plan.get("format") != 2:
            raise ValueError("unsupported plan format")
        backup_dir = args.backup_dir or args.plan.with_suffix(".footers")
        backup_dir.mkdir(parents=True, exist_ok=True)
        for entry in plan["entries"]:
            check_entry(entry, backup_dir)
        for entry in plan["entries"]:
            apply_entry(entry, backup_dir)
        print(f"done; original footers: {backup_dir}", flush=True)
    else:
        if not args.roots:
            parser.error("at least one benchmark root is required for a scan")
        make_plan(args.roots, args.scanner, args.plan)


if __name__ == "__main__":
    main()
