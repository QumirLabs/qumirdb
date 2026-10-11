# Automatic scan parallelism

The default execution mode is threaded, with one worker per logical CPU reported
by `std::thread::hardware_concurrency()` (at least one if unknown). CLI execution
and native plan-export requests share these defaults. No scheduler flags are
needed; `--scheduler` and `--scheduler-workers` remain explicit overrides.

Omitting `--scan-tasks` enables per-source sizing in the threaded scheduler.
An explicit positive value is a cap for every source, including `1` to keep
scans serial. The single scheduler defaults to one scan task. Sources without
row-group metadata keep their original single source task.

For each source, the lowerer first prunes columns and row groups. Let `R` be the
remaining row count, `B` the uncompressed bytes of selected Parquet columns,
`G` the remaining row-group count and `W` the worker count. The automatic cap is:

```
size_tasks = max(1, ceil(R / 1,048,576), ceil(B / 67,108,864))
budget = W if the source feeds a join, otherwise 2 * W
max_tasks = min(size_tasks, G, budget)
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

Splits retain the existing contiguous row-group ranges. For a cap `M`, each
range contains at most `ceil(G / M)` groups, so the actual lane count can be
less than `M`. For example, 59 groups capped at 18 produce 15 lanes. Uneven
row-group sizes can still cause uneven work.

## Scan lanes and join partitions

A hash-shuffled join can take `M_left` and `M_right` input lanes and produce `N`
join partitions. Each key maps to one output partition independently of its
source lane. The two shuffle connections create `(M_left + M_right) * N`
queues. Increasing scan lanes therefore increases graph and queue costs even
when `N` stays constant.

Automatic join partitioning remains
`max(1, min(max(M_left, M_right), max(1, W / 2)))`.
`--shuffle-partitions` overrides this count and may choose `N` larger than the
input lane counts. It also affects other shuffled operators. Scan sizing does
not change this rule; size-based join partitioning would need a separate
estimate of build/probe work and partition memory.

## DuckDB comparison

Inspected the local DuckDB checkout at commit
`044a04a7cd39e6e8235f756597ae42dde084e5e5` and checked the corresponding upstream
implementation. For one Parquet file, its source concurrency limit is the
number of row groups (at least one). Multiple files allow the scheduler's full
concurrency. The pipeline then applies source, intermediate-operator and sink
limits and caps the result at `threads`. It launches one pipeline task per
allowed thread. This path does not use projected bytes or a rows-per-task
threshold to choose the Parquet task count.

Row groups are assigned dynamically: a task receives the next group from the
shared file state and can request another after finishing. A larger work pool
therefore does not require a larger number of pipeline tasks. The relevant
code is
[ParquetMultiFileInfo::MaxThreads and ParquetReader::TryInitializeScan](https://github.com/duckdb/duckdb/blob/main/extension/parquet/parquet_multi_file_info.cpp)
and
[Pipeline::TryGetMaxThreads and LaunchScanTasks](https://github.com/duckdb/duckdb/blob/main/src/parallel/pipeline.cpp).

Native DuckDB tables have a size-based source limit:
`floor(total_rows / row_group_size) + 1`, followed by the same pipeline and
thread caps. The default native row-group size is 122,880 rows. They also assign
row groups dynamically rather than pinning a range to each task. See
[DataTable::MaxThreads](https://github.com/duckdb/duckdb/blob/main/src/storage/data_table.cpp),
[the default row-group size](https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/storage/storage_info.hpp),
and
[RowGroupCollection::NextParallelScan](https://github.com/duckdb/duckdb/blob/main/src/storage/table/row_group_collection.cpp).

DuckDB's ordinary in-memory hash join merges local build collections into one
hash table and probes that table concurrently. It does not use qdb's dense
graph of scan-to-join queues. Its initial radix partition count is 16 below
100 threads and 32 otherwise; these partitions are distinct from pipeline
tasks. For an external join, it can increase the radix bits when the largest
partition will not fit in the memory reservation, except in a heavily skewed
case. The repartition estimate aims at one quarter of the reservation per
partition. See
[PhysicalHashJoin::Finalize](https://github.com/duckdb/duckdb/blob/main/src/execution/operator/join/physical_hash_join.cpp)
and
[JoinHashTable::SetRepartitionRadixBits](https://github.com/duckdb/duckdb/blob/main/src/execution/join_hashtable.cpp).

Dynamic row-group assignment is a separate possible improvement for qdb. It
would help balance row-group work while keeping scan and shuffle lane counts
bounded. The current automatic mode keeps static ranges and chooses their
count from source size and downstream join fan-in.
