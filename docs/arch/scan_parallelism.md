# Automatic scan parallelism

The default execution mode is threaded, with one worker per logical CPU reported
by `std::thread::hardware_concurrency()` (at least one if unknown). CLI execution
and native plan-export requests share these defaults. No scheduler settings are
needed; `[scheduler] mode` and `workers` remain explicit overrides.

Leaving `[scan] max_tasks` at 0 enables per-source sizing in the threaded scheduler.
An explicit positive value is a cap for every source, including `1` to keep
scans serial. The single scheduler defaults to one scan task. Sources without
row-group metadata keep their original single source task.

For each source, the lowerer first prunes columns and row groups. Let `R` be the
remaining row count, `B` the uncompressed bytes of selected Parquet columns
and `W` the worker count. The automatic cap is:

```
size_tasks = max(1, ceil(R / 262,144), ceil(B / 67,108,864))
budget = W if the source feeds a join, otherwise 2 * W
max_tasks = min(size_tasks, budget)
```

The grain settings are `ScanSplit.TargetRowsPerTask` and `TargetBytesPerTask`;
zero disables that size dimension. Missing size estimates leave the source
serial. An empty pruned scan produces an empty source task. Join detection
follows shared CTE materializations as well as ordinary operator inputs.

The worker cap for join inputs limits fan-in and queue overhead. Large scans
without joins can use additional lanes to overlap scan and downstream work.
These are initial heuristics, not a claim of optimality for every query.
`--verbose` prints each automatic cap, actual task count, remaining rows,
row groups and projected bytes.

The row-group count does not cap the tasks. Let `G` be the remaining row-group
count and `M` the cap. With `G >= M`, each split is a contiguous run of at most
`ceil(G / M)` whole groups, so the actual lane count can be less than `M`: 59
groups capped at 18 produce 15 lanes, and uneven group sizes still cause uneven
work. With `G < M`, the splits cut the groups: each split gets `ceil(R / M)`
rows as row ranges (group, first row, row count), computed from the footer
alone, and may end inside one group and continue in the next. Sizing work units
by rows rather than by storage layout is the morsel idea of Leis et al.,
*Morsel-driven parallelism*, SIGMOD 2014, doi:10.1145/2588555.2610507.

A task that starts inside a row group pays for it. Without a Parquet page index
the reader cannot jump to a row: every column of the task reads its dictionary
page and skips the rows of the page that holds its first row. Pages that end
before the range are dropped without decompression. The grain of 262,144 rows
keeps that cost small next to the rows a task reads.

## Scan lanes and join partitions

A hash-shuffled join can take `M_left` and `M_right` input lanes and produce `N`
join partitions. Each key maps to one output partition independently of its
source lane. The two shuffles create `(M_left + M_right) * N`
queues. Increasing scan lanes therefore increases graph and queue costs even
when `N` stays constant.

Automatic join partitioning remains
`max(1, min(max(M_left, M_right), max(1, W / 2)))`.
`[shuffle] partitions` overrides this count and may choose `N` larger than the
input lane counts. It also affects other shuffled operators. Scan sizing does
not change this rule; size-based join partitioning would need a separate
estimate of build/probe work and partition memory.
