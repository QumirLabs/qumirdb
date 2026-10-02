# Recompute Parquet NDV metadata

`tbl2parquet` caps its distinct-value sets at 5,000,000 numeric values or
1,024 strings. Above the cap, the embedded `stats` JSON contains a lower
bound, with `ndv_exact: false`. `ndv_scan.cpp` scans those columns and uses
HyperLogLog with 2^18 registers to estimate NDV. Its expected relative
standard error is about 0.2%; estimates remain marked inexact.

Build the scanner with the Arrow and Parquet libraries used by qdb:

```sh
c++ -std=c++23 -O3 -I/opt/homebrew/include tools/ndv_scan.cpp \
  -L/opt/homebrew/lib -lparquet -larrow -o build/bin/qdb_ndv_scan
```

Scan only the main SF1, SF10 and SF100 directories, and save a plan:

```sh
python3 tools/recompute_parquet_ndv.py \
  --plan /private/tmp/qdb_ndv_main_plan.json \
  ~/Projects/tpch ~/Projects/tpcds
```

The scan leaves data unchanged. Review the printed changes, then apply:

```sh
python3 tools/recompute_parquet_ndv.py \
  --plan /private/tmp/qdb_ndv_main_plan.json --apply
```

Apply checks every footer against the scan plan before writing. It saves the
original footer bytes in a sibling `.footers` directory, replaces only the
custom stats value without changing the file size, and validates Parquet
metadata after each write. A repeated apply recognizes already updated files.
Keep the plan and footer backups together if the original metadata may need
to be restored.
