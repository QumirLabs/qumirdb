# TPC-DS benchmark queries

`run_tpcds.sh` runs the SQL queries in [`sql/`](sql/) via `qdb --sql`.

Unlike the TPC-H harness there is **no templating yet**: the query files carry
concrete literals (dates, filter values) and are run as-is — no `params/`, no
placeholder substitution. Tables are referenced by bare name and resolved by
`qdb --data <dir>` to `<dir>/<table>.parquet`. Parameterization per scale factor
can be layered on later, mirroring `../../tpch/run_tpch.sh`.

The queries are the same set mirrored in `test/regtest/cases/tpcds/` (q01–q99).

## Usage

```sh
# base_dir holds pq1/, pq10/, pq100/ subdirs of parquet files
./run_tpcds.sh /path/to/tpcds            # all discovered scales
./run_tpcds.sh /path/to/tpcds 1          # scale factor 1 only
./run_tpcds.sh /path/to/tpcds 1 6,19,42  # a subset of queries
./run_tpcds.sh /path/to/tpcds 1 14       # all split parts for q14
./run_tpcds.sh /path/to/tpcds 1 14_1     # one split query part

# threaded execution
QDB_ARGS='--scheduler:mode=threaded --scheduler:workers=4' ./run_tpcds.sh /path/to/tpcds 1
```

Per-query timing and status print to stdout; full qdb output goes to
`$OUT_DIR/tpcds_sql_sf<N>.log` (default `OUT_DIR=./results`).

Many queries still fail — at the parser, planner, or kernel-compilation stage.
See `../../../tpcds.md` for the current coverage breakdown.
