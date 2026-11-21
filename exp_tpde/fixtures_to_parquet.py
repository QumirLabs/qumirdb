#!/usr/bin/env python3
"""Converts regtest JSON fixtures to <table>.parquet through the duckdb CLI.

usage: fixtures_to_parquet.py <suite dir> <out dir> [--dates]
--dates writes i32 columns named *date as DATE (TPC-H stores DATE32).
"""
import json
import os
import re
import subprocess
import sys

TYPES = {
    "i8": "TINYINT", "i16": "SMALLINT", "i32": "INTEGER", "i64": "BIGINT",
    "u8": "UTINYINT", "u16": "USMALLINT", "u32": "UINTEGER", "u64": "UBIGINT",
    "f32": "FLOAT", "f64": "DOUBLE", "string": "VARCHAR", "bool": "BOOLEAN",
}


def parse_schema(schema):
    cols = []
    for m in re.finditer(r"\((\w+) (<named Nullable \[(\w+)\]>|(\w+))\)", schema):
        name = m.group(1)
        base = m.group(3) or m.group(4)
        cols.append((name, base, m.group(3) is not None))
    return cols


def literal(value, sqltype):
    if value is None:
        return "NULL"
    if sqltype == "VARCHAR":
        return "'" + str(value).replace("'", "''") + "'"
    if sqltype == "BOOLEAN":
        return "TRUE" if value else "FALSE"
    if sqltype == "DATE":
        return f"(DATE '1970-01-01' + INTERVAL {int(value)} DAY)::DATE"
    return repr(value)


def main():
    suite, out = sys.argv[1], sys.argv[2]
    dates = "--dates" in sys.argv
    os.makedirs(out, exist_ok=True)
    manifest = json.load(open(os.path.join(suite, "suite.json")))
    sql = []
    for table, fixture in manifest["fixtures"].items():
        if "json" not in fixture:
            continue
        data = json.load(open(os.path.join(suite, fixture["json"])))
        cols = parse_schema(data["schema"])
        types = []
        for name, base, _ in cols:
            t = TYPES[base]
            if dates and base == "i32" and name.endswith("date"):
                t = "DATE"
            types.append(t)
        sql.append(f"CREATE TABLE {table} (" + ", ".join(f"{n} {t}" for (n, _, _), t in zip(cols, types)) + ");")
        rows = ["(" + ", ".join(literal(v, t) for v, t in zip(row, types)) + ")" for row in data["rows"]]
        if rows:
            sql.append(f"INSERT INTO {table} VALUES " + ", ".join(rows) + ";")
        groups = fixture.get("parquet", {}).get("row_group_rows", 1 << 20)
        sql.append(f"COPY {table} TO '{os.path.join(out, table)}.parquet' (FORMAT parquet, ROW_GROUP_SIZE {groups});")
    subprocess.run(["duckdb", "-c", "\n".join(sql)], check=True)


if __name__ == "__main__":
    main()
