#!/usr/bin/env python3
"""Generates the data of the fuzz targets from the repository.

tables <repo> <out.inc>  C++ initializers {name, schema} of every regression
                         fixture table and of fuzz/tables/*.json.
corpus <repo> <out-dir>  One seed file per distinct SQL query of the regression,
                         TPC-H, TPC-DS and ClickBench suites.
"""

import hashlib
import json
import shutil
import sys
from pathlib import Path

QUERY_DIRS = (
    "test/regtest/cases",
    "benchmark/tpch/queries/sql",
    "benchmark/tpcds/queries/sql",
    "benchmark/clickbench/queries/sql",
)


def schema_of(path):
    return json.loads(path.read_text(encoding="utf-8"))["schema"]


def tables(repo, out):
    entries = set()
    for suite in sorted((repo / "test/regtest/cases").rglob("suite.json")):
        manifest = json.loads(suite.read_text(encoding="utf-8"))
        fixtures = manifest.get("fixtures", {})
        bindings = [manifest.get("tables", {})]
        bindings += [case.get("tables", {}) for case in manifest.get("cases", {}).values()]
        for table, fixture in (pair for binding in bindings for pair in binding.items()):
            # Committed Parquet fixtures carry their schema in the file footer only.
            json_path = fixtures.get(fixture, {}).get("json")
            if json_path:
                entries.add((table, schema_of(suite.parent / json_path)))
    for path in sorted((repo / "fuzz/tables").glob("*.json")):
        entries.add((path.stem, schema_of(path)))
    # json.dumps of ASCII text is a valid C++ string literal.
    text = "".join(f"{{{json.dumps(name)}, {json.dumps(schema)}}},\n" for name, schema in sorted(entries))
    Path(out).write_text(text, encoding="utf-8")


def corpus(repo, out):
    out = Path(out)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    for directory in QUERY_DIRS:
        for path in sorted((repo / directory).rglob("*.sql")):
            data = path.read_bytes()
            (out / hashlib.sha1(data).hexdigest()).write_bytes(data)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("tables", "corpus"):
        sys.exit(__doc__)
    command, repo, out = sys.argv[1:]
    {"tables": tables, "corpus": corpus}[command](Path(repo), out)


if __name__ == "__main__":
    main()
