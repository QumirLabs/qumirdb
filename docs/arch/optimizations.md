# Optimizations

Every rewrite qdb performs, in the order it happens: during plan construction,
in the pipeline of plan passes, in the CTE phase that runs after them, and at
lowering/execution time outside the plan altogether. Each entry gives a minimal
query and the plan before and after.

## 1. During plan construction

These run during logical plan construction, before the pipeline passes. The "before" shapes below are what the
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
aggregate keys=[a, gb_0]                project (a, col1, col2)
└─ project (a, gb_0 = a + 1)            └─ aggregate keys=[a] aggs=[count_0=count]
   └─ source t.parquet                     └─ source t.parquet
```

A literal key is dropped only when another key survives, so `GROUP BY TRUE` over
an empty input does not silently turn into a global aggregate.

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
└─ project (c*2+3, c+10)                             aggs=[sum_0=sum, count_0=count]
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
         └─ aggregate keys=[v.a] aggs=[sum_0=sum, count_0=count]
            └─ filter (== |v.a| |v.a|)
               └─ source v.parquet
```

The correlated predicate is replaced by an equi-join on the correlation column,
so the subquery is evaluated once per distinct value instead of once per row.
What is left of it inside the subquery is `v.a = v.a`: the outer column was
renamed onto the subquery's own one. `SimplifySelfEquality` (§2.3) removes it.

## 2. Pipeline passes

The pass order below is the per-plan optimizer sequence. Passes are
idempotent; `AnnotateTypes` re-runs after each structural rewrite.

### 2.1 SimplifyOuterJoins

Outer-join simplification, from Galindo-Legaria and Rosenthal,
[Outerjoin Simplification and Reordering for Query Optimization (ACM TODS 22(1),
1997)](https://doi.org/10.1145/244810.244812); PostgreSQL does the same in
`reduce_outer_joins`. A predicate is *null-rejecting* for a set of columns when
it cannot be TRUE on a row where all of those columns are NULL. The row an outer
join adds for an unmatched row of one input has NULL in every column of the
other input. If a null-rejecting predicate must hold above the join, it drops
all such rows, so the join need not produce them:

| join | the predicate rejects NULLs of | becomes |
|---|---|---|
| `A LEFT JOIN B` | `B` | `A JOIN B` |
| `A RIGHT JOIN B` | `A` | `A JOIN B` |
| `A FULL JOIN B` | `A` | `A LEFT JOIN B` |
| `A FULL JOIN B` | `B` | `A RIGHT JOIN B` |
| `A FULL JOIN B` | `A` and `B` | `A JOIN B` |

The narrower join returns the rows of the outer join without the
null-extended rows that the predicate drops. Nothing else changes: duplicates,
NULL keys, matched rows and empty inputs behave as before, and the predicate
stays in the plan.

The pass runs after column qualification and typing, before
`FlattenInnerJoins`. A join it makes inner then belongs to the surrounding inner
region: DPccp can reorder it, predicate pushdown and equi-key extraction treat
it as an inner join, and runtime filters can enter it. Typing runs again after a
rewrite, because the columns of an input that is no longer null-extended may
become non-nullable.

```sql
SELECT t.c, u.d FROM t LEFT JOIN u ON t.a = u.a WHERE u.d <> 'p';
```

```
before                              after
project (c, d)                      project (c, d)
└─ filter (!= |u.d| "p")            └─ join inner [t.a = u.a]
   └─ join left [t.a = u.a]            ├─ source t.parquet AS t
      ├─ source t.parquet AS t         └─ filter (!= |u.d| "p")
      └─ source u.parquet AS u            └─ source u.parquet AS u
```

The pass walks the plan top-down and carries the predicates that every row of
the current subtree must satisfy to reach the result. A `WHERE` filter adds its
predicate. An inner join passes the predicates from above, its `ON` condition
and its keys to both inputs: the condition filters the pairs. A LEFT or RIGHT
join passes the predicates from above to its preserved input, and its own
condition to the null-extended input, where a row that matches nothing is never
output. A semi or anti join passes the predicates from above to the input it
returns; a semi join also passes its condition to both inputs, an anti join
only to the input it does not return. A FULL join passes nothing. So a join made
inner can make the joins under it inner: in

```sql
SELECT ... FROM a LEFT JOIN b ON a.x = b.x LEFT JOIN c ON b.y = c.y WHERE c.z = 1;
```

the `WHERE` makes the upper join inner, and its `ON` then rejects the NULLs of
`b`. Projections, aggregates, windows, limits and unions stop the walk, so a
predicate above a derived table does not narrow a join inside it. The usual
case is decorrelation (§1.5): the comparison with a scalar subquery value
rejects its NULL, and the decorrelation LEFT join becomes inner (TPC-H q2, q17,
q20; TPC-DS q1, q6, q81).

The null-rejection test follows the NULL semantics of the kernels, which
evaluate an operator, a cast or a function on a NULL argument as NULL, except
for the calls with their own NULL handling. It accepts:

- comparisons, arithmetic, casts, `LIKE` and functions over a column of the
  set, nested to any depth;
- `x IN (...)` when the operand `x` is NULL;
- `IS NOT NULL` of such an expression, and `NOT` of such an expression, which
  is NULL as well;
- `p AND q` when `p` or `q` rejects, `p OR q` only when both do.

It refuses `IS NULL`, `coalesce`, `ifnull`, `nvl`, `nullif`, `CASE`, an `IN`
list that has the column only among its items, `NOT` over anything that may be
TRUE or FALSE on a NULL argument, and any expression it does not know. A
refused predicate keeps the join outer. `IS NULL` on a join key is the
anti-join pattern that `RewriteOuterJoinAsAnti` handles at the end of the
pipeline.

### 2.2 FlattenInnerJoins

Only with the cost-based optimizer (default; `--nocbo` disables it). Collapses a
left-deep inner-join chain into one region so DPccp can reassociate it freely.

### 2.3 SimplifySelfEquality

A filter that compares a column with itself only rejects NULLs, but the
estimator would cost it as an equality of two columns, `1/NDV`. Decorrelation
leaves such a comparison in every correlated scalar subquery (§1.5), so the pass
runs before predicate pushdown and the first `EstimateStats`:

| column | `col = col` becomes |
|---|---|
| non-nullable | `TRUE`; a filter left with nothing else is removed |
| nullable | `col IS NOT NULL` |
| float | unchanged: `NaN = NaN` is false |

```sql
SELECT t.a FROM t WHERE t.c > (SELECT avg(v.e) FROM v WHERE v.a = t.a);
```

```
before                                    after (v.a is non-nullable)
aggregate keys=[v.a] aggs=[...]           aggregate keys=[v.a] aggs=[...]
└─ filter (== |v.a| |v.a|)                └─ source v.parquet AS v
   └─ source v.parquet AS v
```

Only plain column references are matched, and only along the AND/OR spine of a
filter predicate. There a NULL result and FALSE both drop the row, so
`(v.a = v.a AND p) OR q` may become `(v.a IS NOT NULL AND p) OR q`. Under `NOT`
or any other operator the comparison is kept: `NOT (NULL = NULL)` is unknown,
while `NOT (v.a IS NOT NULL)` would be true.

A comparison that is kept is estimated by `EstimateStats` as the non-NULL
fraction of the column, never through its NDV.

### 2.4 PushDownPredicates

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
: from
`(a=1 AND x) OR (a=1 AND y)` it extracts the common `a=1` so it can sink.

**Into UNION ALL branches, with dead-branch elimination.** `ProcessUnion`
 rewrites each pushed conjunct into the branch's own column
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

### 2.5 ReorderJoins

Turns a cross-join chain into a connected order, threading equalities down the
chain; with the CBO on, DPccp picks the order by estimated cost using
`EstimateStats` cardinalities. Every join it considers is estimated by the
rules that estimate the plan, residuals included, and a set of relations keeps
the largest estimate of its splits. See [join_reorder.md](join_reorder.md).

Column demand is propagated before cost-based enumeration, so unused source
payload does not inflate a join input's width. Predicate-only columns remain
available to the scan and filter but do not widen the filter's output. Columns
needed by join equalities and final results remain demanded. Pruning runs again
after rewrites and type annotation restore the operator schemas.

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

### 2.6 ExtractEquiJoins

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
condition by `DeriveSideFilter` : in every disjunct it keeps
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

### 2.7 PushDownSemiJoins

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

### 2.8 PushDownLimits

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

### 2.9 ApplyTopSort

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

### 2.10 ApplyColumnPruning

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

Runs once after every plan (main plus each definition) has been optimized.

### 3.1 PushConsumerPredicatesIntoDefinitions / PropagateCteDemands

A predicate that every consumer applies is pushed into the definition, and the
definition's output schema is narrowed to the union of what consumers demand.

### 3.2 ApplyLateMaterialization

After CTE demand is known, QDB knows the final columns for each plan. For a
wide query with a small limit, QDB can read only the filter and sort columns
first. It fetches the other columns after the limit. The default limit
threshold is 100 rows. The eager scan estimate must be at least 2x the late
estimate. See [late_materialization.md](late_materialization.md).

### 3.3 ChooseCteReuse / ApplyCteReuse

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

### 4.1 Parquet row-group pruning

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

See [predicate_pushdown.md](predicate_pushdown.md#parquet-row-group-pruning).

### 4.2 Scan splitting

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

### 4.3 Hash-shuffled grouped aggregate

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

### 4.5 Parallel grouping sets

ROLLUP/GROUPING SETS over parallel input always use `partial → shuffle →
combine`, keyed by a synthetic `__grouping_id__` so masked-NULL sets stay
distinct. Unlike the global cascade this one is not behind a flag.

### 4.6 Asymmetric hash join

The inner join starts symmetric — both sides build and probe — and switches to
probe-only streaming for the remaining side as soon as one input is exhausted,
which is normally the smaller one. This adaptation requires no cardinality
estimate. Lowering can instead choose build-first execution when an input is
estimated much smaller or a runtime filter requires its completion first.

LEFT/RIGHT outer joins can build their preserved side, stream the other input,
and mark each build row only after its residual accepts a match. At stream EOF,
unmarked build rows are emitted once with NULLs for the absent side. Lowering
chooses this path when the preserved side is estimated no larger than the other
input, or when a residual or runtime filter requires it. Building first delays
the probe input but avoids retaining and hashing it; other outer joins keep
the symmetric path. A FULL join with a residual marks matched rows on both
sides of that path. See [join.md](join.md).

```
SELECT big.c, u.d FROM big, u WHERE big.a = u.a;   -- threaded, 4 scan tasks

conn join-left-input   one-to-one   src=4 dst=4     -- big: 4 lanes
conn join-right-input  one-to-one   src=1 dst=1     -- u: 1 lane
conn join-left-shuffle hash-shuffle src=4 dst=2
conn join-right-shuffle hash-shuffle src=1 dst=2
```

### 4.7 Kernel fusion and the JIT object cache

All kernels of one query are compiled as a single fused module, with shared
helpers deduplicated. This shares compilation without removing operator/task
boundaries. The persistent JIT object cache reuses eligible objects, but
planning, frontend work, query-specific emission, and linking still remain.
See [kernel-compilation-phases.md](kernel-compilation-phases.md).

### 4.8 Arena allocation for string aggregation keys

Group keys of string type are copied into a bump arena owned by the hash table
instead of individually allocated, so building a table over many distinct string
keys does not turn into a malloc storm. See [aggregation.md](aggregation.md).

## 5. Helpers that are not optimizations

- `ConstFold` is a helper, not a pipeline pass. UNION ALL pushdown uses it
  after substituting closed projections to decide whether a branch is dead,
  needs the filter, or trivially satisfies it. `EstimateStats` uses it for
  constant comparison bounds. An expression it cannot type or evaluate is
  returned unfolded, so folding never fails a query. It evaluates through the
  Qumir VM, as the kernels compute: an expression over Qumir's own types, such
  as integer arithmetic on a date literal already folded to a number, is
  compiled alone, while one that uses a qdb type, function or operator is
  compiled with the qumirdb module, whose composition costs most of a
  compilation (about 13 ms against 0.05 ms).
- `AssignSourceAliases`, `QualifyColumns`, `AnnotateTypes`,
  `CoerceSetOpBranches`, `unbound_vars`, `typing` — normalization and checking.
- `EstimateStats` — produces the row counts,
  NDVs and selectivities that ReorderJoins and ChooseCteReuse consume. Parquet
  files may carry NDV and equi-depth histograms in a custom footer key.

### Filter selectivity

A filter keeps the input rows for which its predicate is TRUE. `EstimateStats`
estimates that fraction from the statistics of the filter's input: row count,
min/max, null count, NDV, and an equi-depth histogram, as introduced by
Piatetsky-Shapiro and Connell in [Accurate Estimation of the Number of Tuples
Satisfying a Condition (SIGMOD 1984)](https://doi.org/10.1145/602259.602294).
The assumptions are those of System R, Selinger et al. in [Access Path Selection
in a Relational Database Management System (SIGMOD 1979)](https://doi.org/10.1145/582095.582099):
predicates on different columns are independent, values are uniform within a
bucket, and a predicate the statistics cannot answer gets a default, 0.5 here.

- A comparison of a column with a constant reads the histogram. The constant
  may be a closed expression such as `date '1995-01-01' + interval '1' year`;
  it is folded to a number once per query, and the plan keeps the expression.
  `<` and `>=` count the rows strictly below the constant, so a heavy value
  that repeats as a bucket boundary falls on the correct side.
- The bounds on one column form one range rather than two independent
  predicates, whose product can be far from the narrow range they describe.
  The histogram cannot resolve a range narrower than a bucket, so a range that
  can hold a value of the column keeps at least `1/NDV`, but never more than
  either bound alone: adding a conjunct never raises an estimate. A range that
  is empty or lies outside min/max keeps nothing.
- `col = c` is `1/NDV`, or nothing when `c` lies outside min/max. `col IN (...)`
  counts the distinct non-NULL constants inside min/max. `NOT IN` is estimated
  as the chain of `<>` it stands for, `(1 - 1/NDV)` per constant, so it cannot
  reach zero from a list longer than the NDV.
- A NULL compares as UNKNOWN, so a comparison, `=`, `<>`, a range or an `IN`
  list, keeps only non-NULL rows. The NDV and the histogram describe the
  non-NULL values, and the share of them a comparison keeps is scaled by the
  column's non-NULL fraction where the null count describes the rows, as for
  `IS NULL` below.
- `col IS NULL` is the null fraction of the column, used only directly above
  the scan: the null count describes the table and is not rescaled by the
  operators in between.
- `NOT p` keeps the rows where `p` is FALSE. That is the complement of TRUE only
  when `p` cannot be NULL; otherwise the UNKNOWN rows are not known, and
  `NOT p` gets the default.
- `x < y` for two columns takes them as independent. The fraction is the mean,
  over the rows of `y`, of the fraction of `x` below them: each equi-depth
  bucket of `y` holds an equal share of its rows and contributes the mean of
  `x`'s histogram at its two boundaries.

### Join cardinality

An equi-join is estimated from each input's row count and the NDV, min/max and
histogram of its key. Selinger et al. (cited above) divide the cross product
by the larger NDV of the two keys. That division rests on the containment
assumption, which Swami and Schiefer state and analyze in [On the Estimation
of Join Result Sizes (EDBT 1994)](https://doi.org/10.1007/3-540-57818-8_58):
every value of the key with fewer distinct values occurs in the other key, so
each of its rows meets `rows / NDV` rows of the other side. QDB takes the NDVs
of the tables, not of the filtered rows: a filter reduces a side's rows but not
its key domain. Otherwise a filtered dimension that keeps more keys than the
fact references would look like a join that keeps every fact row.

The two keys rarely span the same values. A TPC-DS sales table references five
years of a date dimension that spans two centuries. Only keys inside the range
both sides span can match. Histograms of two join keys estimate the join from
the parts of the histograms that cover the same values; Ioannidis and
Christodoulakis analyze such estimates in [On the Propagation of Errors in the
Size of Join Results (SIGMOD 1991)](https://doi.org/10.1145/115790.115835).
QDB aligns the two keys on one part, the intersection of their min/max ranges,
and restricts each side to it. A side's domain is its NDV times the fraction of
its rows inside the intersection, read from its histogram:

```text
rows = rows_L(inside) * rows_R(inside) / max(domain_L, domain_R)
```

Per-column statistics do not say where a filter's rows fall: `d_year = 2000`
is correlated with the date key, `d_moy = 12` is not. An unfiltered input
spreads its keys over its range as its histogram says. A filtered input has
fewer keys, and two assumptions place them. Independence of the filter and
the key, the assumption of Selinger et al. carried across the filter, spreads
them over the input's whole range like the unfiltered ones. Containment
carried across the filter places them inside the intersection as far as it
holds them. QDB takes containment, because a query filters a dimension for
the rows it joins: spread over two centuries, the dates a filter keeps would
mostly fall outside the few years the fact references, and the estimate would
be the independence estimate again.
In TPC-DS SF10, `d_year = 2000` keeps 363 of 73,049 dates and `store_sales`
references 1,823 of them, so the join keeps about 363/1,823 of `store_sales`:
5.7M rows against 5.5M actual, where independence gives 143K. The trade-off is
a filter independent of the key that keeps more keys than the intersection
holds: `d_moy = 12` alone is estimated to keep every date the fact references.
That over-estimate is the safer error for a hash join that builds the smaller
estimated input: over-estimating a side can only make it build the other side,
whose estimate is no larger, while an under-estimate makes it build an input
far larger than estimated.

When the ranges coincide, as for most dimension keys, the estimate is the
one of Selinger et al. Without a histogram, min/max only bound a column and the
estimate makes no claim beyond an empty intersection. A join that lowers a
column's NDV, to the matched keys or to its output rows, keeps a selection of
the values its histogram described, without saying where they lie, so the
column keeps its min/max and drops the histogram. Otherwise a date column that
a join cut to one year's rows would still look spread over two centuries.

A join's residual, the part of its condition other than key equalities, keeps
the share of the key matches it accepts. It is estimated by the filter rules
over the joined row and taken as independent of the keys. In TPC-DS SF10,
`inv_quantity_on_hand < cs_quantity` keeps about 5% of the `catalog_sales ⋈
inventory` matches on the item key, read from the two quantity histograms. A
semi or anti join keeps a row whose key matches include one the residual
accepts: with its matches taken as independent, a row with `m` matches loses
them all with probability `(1 - s)^m`.
