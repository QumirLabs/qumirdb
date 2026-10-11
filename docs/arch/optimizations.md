# Optimizations

Every rewrite qdb performs, in the order it happens: during plan construction,
in the pipeline of plan passes, in the CTE phase that runs after them, and at
lowering/execution time outside the plan altogether. Each entry gives a minimal
query and the plan before and after.

## Reproducing the plans

```sh
QDB_DUMP_PASSES=1 ./build/bin/qdb --sql --data <dir> -i q.sql   # plan after every pass
./build/bin/qdb --sql --verbose --data <dir> -i q.sql           # logical + runtime plan + scheduler graph
```

All examples below use four tiny Parquet tables:

| table | columns | rows |
|---|---|---|
| `t` | `a int64, b string, c int64` | 6 |
| `u` | `a int64, d string` | 4 |
| `v` | `a int64, e int64` | 3 |
| `big` | `a int64, c int64` | 40 rows in 4 row groups |

Plans are printed by `PrintPlanTree` (`qdb/plan/plan_print.cpp`); source paths
are shortened here. `rows≈`/`cost≈` annotations appear once `EstimateStats` has
run, and are elided below where they are not the point.

## 1. During plan construction

These run inside `qdb/plan/build.cpp`, so `QDB_DUMP_PASSES` already shows their
result under `===== initial =====`. The "before" shapes below are what the
straightforward translation would produce; they never exist as a plan.

### 1.1 Global aggregate as a constant group key

An aggregate without GROUP BY becomes a grouped aggregate over a synthetic
constant key, so one code path serves both cases.

```sql
SELECT sum(c) FROM t;
```

```
before                          after
aggregate aggs=[sum(c)]         aggregate keys=[__group__] aggs=[sum_0=sum]
└─ source t.parquet             └─ project (c, __group__)      -- __group__ = 1
                                   └─ source t.parquet
```

### 1.2 GROUP BY key narrowing

Keys that are literals, or integer affine expressions `x + c`, `c - x`, `x * c`
over a column that is already a key, are removed: they cannot split a group.
The expression stays in SELECT/HAVING and is computed once per output group.
See [logical_plan_build.md](logical_plan_build.md#functionally-dependent-group-key-narrowing).

```sql
SELECT a, a + 1, count(*) FROM t GROUP BY a, a + 1;
```

```
before                                  after (===== initial =====)
aggregate keys=[a, gb_1]                project (a, col1, col2)
└─ project (a, gb_1 = a + 1)            └─ aggregate keys=[a] aggs=[count_0=count]
   └─ source t.parquet                     └─ source t.parquet
```

A literal key is dropped only when another key survives, so `GROUP BY 1` over an
empty input does not silently turn into a global aggregate.

### 1.3 Affine integer aggregates

Aggregates over `a * x + b` (integer `x`, integer literals) are rewritten onto
the base column, and the resulting base slots are deduplicated:

```
sum(a*x + b)   =>  a * sum(x) + b * count(x)
avg(a*x + b)   =>  (a * sum(x) + b * count(x)) / count(x)
count(a*x + b) =>  count(x)
min/max(a*x+b) =>  a * min/max(x) + b        (min and max swap for a < 0)
```

```sql
SELECT sum(c * 2 + 3), avg(c + 10) FROM t;
```

```
before                                            after (===== initial =====)
aggregate aggs=[sum(c*2+3), sum(c+10),            project (col0, col1)
                count(c+10)]                      └─ aggregate keys=[__group__]
└─ project (c*2+3, c+10)                             aggs=[sum_0=sum, count_1=count]
   └─ source t.parquet                               └─ project (c, __group__)
                                                        └─ source t.parquet
```

Four aggregate arguments collapse into two slots over the raw column, and no
per-row arithmetic is materialized at all.

### 1.4 count(distinct x)

```sql
SELECT count(distinct b) FROM t;
```

```
after (===== initial =====)
project (col0)
└─ aggregate keys=[__group__] aggs=[count_0=count]
   └─ project (b, __group__)
      └─ aggregate keys=[b]          -- distinct as a grouping, no per-group state
         └─ source t.parquet
```

### 1.5 Decorrelation

`IN`/`EXISTS` become semi/anti joins; a correlated scalar subquery becomes a
group-by over the inner relation plus a LEFT join (Apply elimination).
See [decorrelation.md](decorrelation.md).

```sql
SELECT t.a FROM t WHERE t.a IN (SELECT u.a FROM u);
```

```
after (===== initial =====)
project (a)
└─ join left_semi residual (== |t.a| __qdb_in__a)
   ├─ source t.parquet
   └─ project (__qdb_in__a)
      └─ source u.parquet
```

```sql
SELECT t.a FROM t WHERE t.c > (SELECT avg(v.e) FROM v WHERE v.a = t.a);
```

```
after (===== initial =====)
project (a)
└─ filter (> |t.c| __scalar_0__)
   └─ join left [t.a = __corr_0_0__]
      ├─ source t.parquet
      └─ project (__corr_0_0__, __scalar_0__)
         └─ aggregate keys=[v.a] aggs=[sum_0=sum, count_1=count]
            └─ filter (== |v.a| |v.a|)
               └─ source v.parquet
```

The correlated predicate is replaced by an equi-join on the correlation column,
so the subquery is evaluated once per distinct value instead of once per row.

## 2. Pipeline passes

Order and re-annotation points live in `qdb/plan/pipeline.cpp`. Passes are
idempotent; `AnnotateTypes` re-runs after each structural rewrite.

### 2.1 FlattenInnerJoins — `plan/passes/flatten_joins.cpp`

Only with the cost-based optimizer (default; `--nocbo` disables it). Collapses a
left-deep inner-join chain into one region so DPccp can reassociate it freely.

### 2.2 PushDownPredicates — `plan/passes/equijoin.cpp`

A conjunction above a join is split per side: conjuncts referencing only one
side sink onto that side, the rest stays above as a residual. This is the
"correlated filter" case — the shared condition is not lost, it is left on top.
See [predicate_pushdown.md](predicate_pushdown.md).

```sql
SELECT t.a, u.d FROM t, u WHERE t.a = u.a AND t.c > 20 AND u.d <> 'p';
```

```
before                                          after
filter (&& (&& (== t.a u.a) (> t.c 20))         filter (== |t.a| |u.a|)
        (!= u.d "p"))                           └─ join inner
└─ join inner                                      ├─ filter (> |t.c| 20)
   ├─ source t.parquet AS t                        │  └─ source t.parquet AS t
   └─ source u.parquet AS u                        └─ filter (!= |u.d| "p")
                                                      └─ source u.parquet AS u
```

A conjunct that mentions both sides cannot sink and stays on top:

```sql
SELECT t.a FROM t, u WHERE t.a = u.a AND t.c > 20 AND t.c + u.a > 25;
```

```
after
filter (&& (== |t.a| |u.a|) (> (+ |t.c| |u.a|) 25))   -- residual, both sides
└─ join inner
   ├─ filter (> |t.c| 20)                             -- sank
   │  └─ source t.parquet AS t
   └─ source u.parquet AS u
```

The pass also pushes into derived tables and factors disjunctions
(`plan/passes/factor_conjuncts.cpp`, `flatten_disjuncts.cpp`): from
`(a=1 AND x) OR (a=1 AND y)` it extracts the common `a=1` so it can sink.

**Into UNION ALL branches, with dead-branch elimination.** `ProcessUnion`
(`equijoin.cpp:365`) rewrites each pushed conjunct into the branch's own column
names, substitutes *closed* projections — a branch column whose expression
references no input, such as a constant tag — and const-folds the result. A
conjunct folding to `false` kills the branch outright; one folding to `true`
needs no filter in that branch. This is the TPC-DS `UNION ALL` over
store/catalog/web sales with a `sale_type` tag:

```sql
SELECT x.a
FROM (SELECT t.a AS a, 's' AS tag FROM t
      UNION ALL
      SELECT u.a AS a, 'c' AS tag FROM u) AS x
WHERE x.tag = 's';
```

```
before                                      after
filter (== |x.tag| "s")                     project (x.a, x.tag)
└─ project (x.a, x.tag)                     └─ project (a, tag)
   └─ union-all                                └─ source t.parquet AS t
      ├─ project (a, tag)
      │  └─ source t.parquet
      └─ project (a, tag)
         └─ source u.parquet
```

The `u` branch, the union and the filter all disappear.

### 2.3 ReorderJoins — `plan/passes/join_order.cpp`, `plan/passes/cbo/dpccp.cpp`

Turns a cross-join chain into a connected order, threading equalities down the
chain; with the CBO on, DPccp picks the order by estimated cost using
`EstimateStats` cardinalities. See [join_reorder.md](join_reorder.md).

```sql
SELECT t.a FROM t, v, u WHERE t.a = u.a AND u.a = v.a;   -- v listed between the joined pair
```

```
before                            after
join inner                        join inner
├─ join inner                     ├─ join inner
│  ├─ source t AS t               │  ├─ source t AS t     (rows≈6)
│  └─ source v AS v               │  └─ source u AS u     (rows≈4)   -- pulled up
└─ source u AS u                  └─ source v AS v        (rows≈3)
```

### 2.4 ExtractEquiJoins — `plan/passes/equijoin.cpp`

Lifts equalities out of the residual filter into join keys, using equivalence
classes: `t.a = u.a AND u.a = v.a` yields a key for the upper join even though
no conjunct mentions `t.a = v.a` directly.

```sql
SELECT t.a FROM t, u, v WHERE t.a = u.a AND u.a = v.a AND t.a > 1;
```

```
before                                        after
filter (&& (== t.a u.a) (== u.a v.a))         join inner [t.a = v.a]
└─ join inner                                 ├─ join inner [t.a = u.a]
   ├─ join inner                              │  ├─ filter (> |t.a| 1)
   │  ├─ filter (> |t.a| 1)                   │  │  └─ source t AS t
   │  │  └─ source t AS t                     │  └─ source u AS u
   │  └─ source u AS u                        └─ source v AS v
   └─ source v AS v
```

Note the filter `t.a > 1` reaching the `t` leaf: the class `{t.a, u.a, v.a}`
also lets a predicate on one member be replayed on the others.

**Derived per-side filters from a disjunction.** A residual conjunct that
touches both sides is additionally *weakened* into a one-sided necessary
condition by `DeriveSideFilter` (`equijoin.cpp:182`): in every disjunct it keeps
only the atoms over that side and re-ORs them. If any disjunct constrains the
side with nothing, no condition can be derived and nothing is pushed. The
original disjunction stays as the join residual — the derived filter is
necessary but not sufficient.

```sql
SELECT t.a FROM t, u
WHERE t.a = u.a
  AND ((t.b = 'x' AND u.d = 'p') OR (t.b = 'y' AND u.d = 'q'));
```

```
before                                            after
filter (&& (== t.a u.a)                           join inner [t.a = u.a] residual
        (|| (&& (== t.b "x") (== u.d "p"))            (|| (&& (== t.b "x") (== u.d "p"))
            (&& (== t.b "y") (== u.d "q"))))            (&& (== t.b "y") (== u.d "q")))
└─ join inner                                     ├─ filter (|| (== |t.b| "x") (== |t.b| "y"))
   ├─ source t.parquet AS t                       │  └─ source t.parquet AS t
   └─ source u.parquet AS u                       └─ filter (|| (== |u.d| "p") (== |u.d| "q"))
                                                     └─ source u.parquet AS u
```

This is the TPC-H Q7 shape:

```sql
AND ((n1.n_name = 'FRANCE' AND n2.n_name = 'UNITED KINGDOM')
  OR (n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'FRANCE'))
```

Each `nation` scan gets `n_name = 'FRANCE' OR n_name = 'UNITED KINGDOM'`, so 23
of 25 nations — and everything joined through them — are dropped before the
join, while the pairing condition is still enforced by the residual above.

### 2.5 PushDownSemiJoins — `plan/passes/join_order.cpp`

A semi join whose keys touch one side of an inner join below it is pushed onto
that side, so the semi filter shrinks the input before the inner join runs.

```sql
SELECT t.a FROM t, v WHERE t.a = v.a AND EXISTS (SELECT 1 FROM u WHERE u.a = t.a);
```

```
before                              after
join left_semi [t.a = u.a]          join inner [t.a = v.a]
├─ join inner [t.a = v.a]           ├─ join left_semi [t.a = u.a]
│  ├─ source t AS t                 │  ├─ source t AS t
│  └─ source v AS v                 │  └─ source u AS u
└─ source u AS u                    └─ source v AS v
```

### 2.6 PushDownLimits — `plan/passes/push_limit.cpp`

`LIMIT` sinks through row-preserving projections down to the source, so the scan
stops early instead of after the whole projection chain.

```sql
SELECT x.a FROM (SELECT t.a AS a, t.c AS c FROM t) AS x LIMIT 2;
```

```
before                    after
limit                     project (a)
└─ project (a)            └─ project (x.a)
   └─ project (x.a)          └─ project (a)
      └─ project (a)            └─ limit
         └─ source t                └─ source t AS t
```

### 2.7 ApplyTopSort — `plan/passes/top_sort.cpp`

`limit(sort(...))` collapses into a single top-K operator, which keeps a bounded
heap instead of materializing and sorting the whole input.

```sql
SELECT a FROM t ORDER BY c DESC LIMIT 2;
```

```
before                                 after
limit 2                                project (a)
└─ sort [__sort_0 desc]                └─ top-sort [__sort_0 desc] limit 2
   └─ project (a, __sort_0)               └─ project (a, __sort_0)
      └─ source t AS t                       └─ source t AS t
```

### 2.8 ApplyColumnPruning — `plan/passes/column_pruning.cpp`

Narrows each operator's required input columns, which for a Parquet source means
fewer column chunks are read. It does not change the tree shape, so it shows up
in the cost annotation rather than in the printed operators:

```sql
SELECT t.a FROM t, u WHERE t.a = u.a;
```

```
before pruning                          after pruning
source t.parquet AS t (cost≈192)        source t.parquet AS t (cost≈48)
source u.parquet AS u (cost≈96)         source u.parquet AS u (cost≈32)
```

`t` has three columns and only `a` survives; the scan cost drops fourfold.

## 3. CTE phase

Runs once after every plan (main plus each definition) has been optimized —
`ApplyPlanPasses` in `qdb/plan/pipeline.cpp`.

### 3.1 PushConsumerPredicatesIntoDefinitions / PropagateCteDemands

A predicate that every consumer applies is pushed into the definition, and the
definition's output schema is narrowed to the union of what consumers demand.

### 3.2 ApplyLateMaterialization — `plan/passes/late_materialization.cpp`

After CTE demand is known, QDB knows the final columns for each plan. For a
wide query with a small limit, QDB can read only the filter and sort columns
first. It fetches the other columns after the limit. The default limit
threshold is 100 rows. The eager scan estimate must be at least 2x the late
estimate. See [late_materialization.md](late_materialization.md).

### 3.3 ChooseCteReuse / ApplyCteReuse — `plan/passes/cte_reuse.cpp`

Cost-based: a definition used once (or cheap enough to recompute) is inlined,
otherwise it stays a materialized `cte-ref` shared by consumers.

```sql
WITH w AS (SELECT a, c FROM t) SELECT w.a FROM w, u WHERE w.a = u.a AND w.c > 20;
```

```
before (single use)                     after
join inner [w.a = u.a]                  join inner [w.a = u.a]
├─ project (w.a)                        ├─ project (w.a)
│  └─ filter (> c 20)                   │  └─ filter (> c 20)
│     └─ cte-ref #0                     │     └─ project (a, c)        -- inlined
└─ source u AS u                        │        └─ source t AS t
                                        └─ source u AS u
```

## 4. Outside the plan pipeline

### 4.1 Parquet row-group pruning — `plan/passes/row_group_predicate.cpp`

`AttachRowGroupPredicates` runs after the CTE phase and attaches a conservative
predicate *hint* to the source; scheduler lowering evaluates it against each
row group's min/max/null metadata and skips whole groups. The row-level filter
is deliberately kept — the hint is allowed to be imprecise.

```sql
SELECT a FROM t WHERE c > 40;
```

```
before                              after
filter (> |t.c| 40)                 filter (> |t.c| 40)
└─ source t.parquet AS t            └─ source t.parquet AS t row-groups (> |t.c| 40)
```

`QDB_DEBUG_ROW_GROUP_PREDICATE=1` reports what was skipped.
See [predicate_pushdown.md](predicate_pushdown.md#parquet-row-group-pruning).

### 4.2 Scan splitting — `scheduler/scan_split.cpp`

With the threaded scheduler a Parquet source is split into up to `--scan-tasks`
independent row-group ranges, which is what creates the parallel lanes every
optimization below builds on. A single-row-group file yields one lane, and the
parallel variants then do not apply.

Without an explicit limit, each source is sized after column and row-group
pruning: one task per 1,048,576 rows or 64 MiB of uncompressed selected-column
data, whichever yields more tasks. The limit is capped at the number of row
groups and at the worker count for sources feeding a join, or twice the worker
count otherwise. Single-threaded execution stays serial by default. Positive
`--scan-tasks` values preserve the explicit limit. See
[Scan parallelism](scan_parallelism.md), including the comparison with DuckDB.

### 4.3 Hash-shuffled grouped aggregate — `scheduler/plan_lowerer.cpp`

Always on for a grouped aggregate over parallel input: the input is shuffled by
group key into partition-local tables, each of which computes complete groups.

```sql
SELECT a, sum(c) FROM big GROUP BY a;    -- --scheduler threaded --scheduler-workers 4 --scan-tasks 4
```

```
single scheduler                     threaded
conn blocking-input   src=1 dst=1    conn aggregate-shuffle-input one-to-one  src=4 dst=4
                                     conn aggregate-shuffle      hash-shuffle src=4 dst=2
                                     conn final-gather           gather       src=2 dst=1
```

### 4.4 Global aggregate cascade — `--cascade-aggregates`, off by default

An ungrouped aggregate over parallel input otherwise gathers every lane into a
*single* aggregate task: the scan is parallel, the fold is not. The flag builds
`partial[N] → gather → combine[1]` instead, where `combine` turns `count` into a
`sum` of per-lane counts.

```sql
SELECT sum(c) FROM big;   -- --scheduler threaded --scheduler-workers 4 --scan-tasks 4
```

```
default                                        --cascade-aggregates
nodes: 11, connections: 4                      nodes: 15, connections: 5
conn blocking-input-gather gather src=4 dst=1  conn aggregate-cascade-input  one-to-one src=4 dst=4
                                               conn aggregate-cascade-gather gather     src=4 dst=1
```

Measured on ClickBench (`hits.parquet`, 100 M rows, 18 workers):
`SELECT AVG(UserID) FROM hits` takes 0.337 s by default and 0.045 s with the
cascade — DuckDB on the same machine takes 0.046 s. The default is off because
the single-aggregate path wins for cheap aggregates over small inputs; on large
scans it costs ~7×.

### 4.5 Parallel grouping sets — `scheduler/plan_lowerer.cpp`

ROLLUP/GROUPING SETS over parallel input always use `partial → shuffle →
combine`, keyed by a synthetic `__grouping_id__` so masked-NULL sets stay
distinct. Unlike the global cascade this one is not behind a flag.

### 4.6 Asymmetric hash join

The inner join starts symmetric — both sides build and probe — and switches to
probe-only streaming for the remaining side as soon as one input is exhausted,
which is normally the smaller one. Nothing is decided by the planner: the
adaptation is a scheduling property, so a wrong cardinality estimate costs
nothing here. See [join.md](join.md).

```
SELECT big.c, u.d FROM big, u WHERE big.a = u.a;   -- threaded, 4 scan tasks

conn join-left-input   one-to-one   src=4 dst=4     -- big: 4 lanes
conn join-right-input  one-to-one   src=1 dst=1     -- u: 1 lane
conn join-left-shuffle hash-shuffle src=4 dst=2
conn join-right-shuffle hash-shuffle src=1 dst=2
```

### 4.7 Kernel fusion and the JIT object cache

All kernels of one query are compiled as a single fused module (shared helpers
are deduplicated), and `QDB_JIT_CACHE_DIR=<dir>` caches the compiled objects
keyed by kernel spec, so a repeated query skips LLVM entirely. Planning plus JIT
is ~0.14 s on a cold cache for a single-aggregate query — the fixed cost that
dominates sub-second queries. See
[kernel-compilation-phases.md](kernel-compilation-phases.md).

### 4.8 Arena allocation for string aggregation keys

Group keys of string type are copied into a bump arena owned by the hash table
instead of individually allocated, so building a table over many distinct string
keys does not turn into a malloc storm. See [aggregation.md](aggregation.md).

## 5. Helpers that are not optimizations

- `plan/passes/const_fold.cpp` — `ConstFold` is a helper, not a pipeline pass.
  Its only production caller is `plan/passes/equijoin.cpp:403`, inside the
  UNION ALL pushdown of §2.2: a conjunct cloned into a branch, with closed
  projections substituted for its columns, is folded to decide whether the
  branch is dead, needs the filter, or trivially satisfies it.
- `AssignSourceAliases`, `QualifyColumns`, `AnnotateTypes`,
  `CoerceSetOpBranches`, `unbound_vars`, `typing` — normalization and checking.
- `EstimateStats` (`plan/passes/estimate_stats.cpp`) — produces the row counts,
  NDVs and selectivities that ReorderJoins and ChooseCteReuse consume. Parquet
  files may carry NDV and equi-depth histograms in a custom footer key.

## 6. Not yet in master

- **Aggregate hash-table pre-sizing from NDV** — sizes the Robin Hood table from
  group-key NDVs instead of growing from 4, and moves the string hash/copy
  primitives into the runtime. On branch `todo-aggregate-presizing`, pending the
  review findings recorded there.
