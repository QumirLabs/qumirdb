# Logical plan build

SQL AST → an unqualified, untyped logical operator tree. The builder is
deliberately naive: it never extracts join keys, pushes predicates, or reorders
joins — those are optimizer passes that run afterwards (see
[join_reorder.md](join_reorder.md), [predicate_pushdown.md](predicate_pushdown.md),
[decorrelation.md](decorrelation.md)).

## Pipeline position

```
parse → BuildPlan → source aliases / column qualification / typing
      → predicate pushdown → estimates → join ordering → equi-key extraction
      → semi/limit/top-sort rewrites → pruning → CTE optimization
      → physical lowering
```

`BuildPlan` produces everything left of `AssignSourceAliases`. Column names are
the raw SQL identifiers (bare or `alias.col`); no types are attached.

## Operators produced

| Operator | Carries |
|---|---|
| `source` | table, alias |
| `filter` | one predicate expression |
| `project` | list of `(name, expression)` |
| `aggregate` | group keys (names), aggregate specs `(name, func, arg-expr\|null)` |
| `join` | left, right, keys `(leftCol,rightCol)`, type, residual expr (null) |
| `window` | partition keys (names), order keys, frame, function specs `(name, func, arg-expr\|null)` |

Join types: `Inner, Left, Right, Full, LeftSemi, RightSemi, LeftAnti, RightAnti`.

## Per-clause mapping

### FROM → left-deep cross-join chain

Comma-separated tables become a left-deep tree of `inner` joins with **empty
keys and no residual** (a pure cross product at build time):

```
FROM a, b, c
=>
join inner            (empty keys)
├─ join inner         (empty keys)
│  ├─ source a
│  └─ source b
└─ source c
```

`a JOIN b ON <cond>` → one `join` whose whole `ON` is the **residual** (key
extraction happens later). `USING (c)` → key `(c, c)`.

### WHERE → conjuncts, decorrelation, filter

`WHERE` is split into top-level `&&` conjuncts. Each conjunct is dispatched:

- `EXISTS / IN` (and negations) → wrap the current node in a semi/anti join
  (see [decorrelation.md](decorrelation.md)).
- a conjunct containing a scalar subquery → `ExtractScalarSubqueries` rewrites it
  and attaches the subquery (cross join or decorrelating LEFT join).
- everything else → collected into a residual list.

The residual list becomes a single `filter` on top of the (possibly
join-wrapped) node:

```
WHERE a.x = b.y AND a.z > 5
=>
filter (&& (== a.x b.y) (> a.z 5))
└─ <FROM tree>
```

### SELECT / GROUP BY / HAVING → project / aggregate

Non-aggregated query → a single `project`.

Aggregated query (`GROUP BY` present, or any aggregate call in SELECT/HAVING):

```
project   (final SELECT list; drops helper columns)
└─ filter (HAVING, if present)
   └─ aggregate keys=[<group keys>] aggs=[<specs>]
      └─ project (only when arguments must be materialized — see below)
         └─ <WHERE tree>
```

`avg(x)` has no kernel; it is rewritten to `sum(x) / cast(count(x) as f64)` —
two specs plus a projection expression.

### Aggregate argument materialization

The aggregate executor requires every aggregate **argument to be a column
reference**. When any argument is a computed expression, an inner `project`
materializes it (and passes the group keys through), and the spec's argument is
replaced by a reference:

```
sum(l_extendedprice * (1 - l_discount))  GROUP BY l_returnflag
=>
aggregate keys=[l_returnflag] aggs=[sum_0=sum(arg_0)]
└─ project (l_returnflag, arg_0 = (* l_extendedprice (- 1 l_discount)))
   └─ ...
```

Plain-column arguments (`sum(l_quantity)`) are passed through unchanged; the
inner project is then omitted unless a global key is needed (below).

### Functionally dependent group-key narrowing

For a plain `GROUP BY`, the builder removes key components that cannot split a
group already defined by another retained component:

```sql
GROUP BY 1, url
-- physical group key: url

GROUP BY client_ip, client_ip - 1, client_ip - 2
-- physical group key: client_ip
```

The current conservative rule recognizes literal keys and integer affine
expressions (`x + c`, `x - c`, `c - x`, and `x * c`) when the base column `x`
is itself a group key. A literal is removed only when another key remains, so a
grouped empty input is not accidentally changed into a global aggregate. The
removed expressions stay in SELECT/HAVING and are evaluated from the retained
key once per output group.

This rewrite intentionally runs inside `BuildSelect`, after GROUP BY aliases
are resolved and before computed keys become materialized `gb_n` columns. A
later plan pass would otherwise see only the synthetic column reference and
would have to reconstruct its dependency through the lower project. In the
current qdb SQL dialect an integer in GROUP BY is a literal expression, not a
SELECT-list ordinal; ordinal support must resolve such references before this
rewrite.

`GROUPING SETS`, `ROLLUP`, and `CUBE` are excluded because their key subsets and
`GROUPING()` bit positions are observable.

### Affine integer aggregates

Before argument materialization, aggregates over one integer column and integer
literals are normalized using the affine form `a * x + b`:

```
sum(a * x + b)  =>  a * sum(x) + b * count(x)
avg(a * x + b)  =>  (a * sum(x) + b * count(x)) / count(x)
count(a * x + b) => count(x)
min(a * x + b)  =>  a * min(x) + b
max(a * x + b)  =>  a * max(x) + b
```

For a negative `a`, `min` and `max` exchange places. The collector deduplicates
the resulting base slots, so a wide select list such as `sum(x + 10),
sum(x + 11), avg(x + 123), avg(x + 1234)` uses only `sum(x)` and `count(x)`
reducers. (`avg` already has no native reducer in qdb.) `count(x)`, not
`count(*)`, preserves SQL NULL semantics; an all-NULL group remains NULL because
the derived value still depends on the nullable base aggregate. The rewrite is
intentionally limited to integer columns. Floating-point aggregates keep their
original row-wise expressions because changing the evaluation or accumulation
order can change rounding.

The rewrite is disabled when a `GROUPING SETS`, `ROLLUP`, or `CUBE` expansion
produces multiple sets. Those kernels mask omitted grouping-key columns; a
computed argument such as `sum(k + 1)` must remain a separate materialized
column so subtotal sets still aggregate the original `k` values.

### Global aggregate (no GROUP BY)

The grouping-key descriptor requires at least one key, so a constant key is
synthesized and dropped by the final project:

```
SELECT sum(l_extendedprice * l_discount)         -- no GROUP BY
=>
project (revenue)
└─ aggregate keys=[__group__] aggs=[sum_0=sum(arg_0)]
   └─ project (arg_0 = (* l_extendedprice l_discount), __group__ = (: 1 i64))
```

### count(distinct col) → double aggregation

There is no distinct kernel. `count(distinct x)` grouped by `G` is realized as a
dedup aggregate (no measures) under a counting aggregate:

```
SELECT p_brand, count(distinct ps_suppkey) GROUP BY p_brand
=>
aggregate keys=[p_brand] aggs=[count_0=count(ps_suppkey)]
└─ aggregate keys=[p_brand, ps_suppkey]            -- dedup, no measures
   └─ ...
```

Only `count(distinct <single column>)` is supported; mixing distinct and
non-distinct aggregates, or distinct over several columns, is rejected.

### SELECT DISTINCT

An `aggregate` with all output columns as group keys and no measures.

### Window functions → window

A window call (`f(...) OVER (PARTITION BY ... ORDER BY ... <frame>)`) is evaluated
**after** WHERE / GROUP BY / HAVING and **before** the final SELECT projection.
Functions that share one identical spec (partition + order + frame) are grouped
into a single `window` node; distinct specs stack into several:

```
project   (final SELECT list; window calls replaced by references)
└─ window  partition=[...] order=[...] fns=[w_0=rank, ...]      -- one per spec
   └─ filter (HAVING, if present)
      └─ aggregate / <WHERE tree>
```

- The node **preserves every input row and column** and appends one output
  column (`w_<n>`) per function; the SELECT expression referencing the window is
  rewritten to that column.
- Aggregates inside a window argument or its PARTITION/ORDER (`sum(sum(x)) OVER`,
  `rank() OVER (ORDER BY sum(x))`) are hoisted into the aggregate below, so by the
  time the window is built its keys/arguments are over the aggregated relation.
- Computed partition/order keys and function arguments are materialized into a
  pass-through `project` below the window (like aggregate argument
  materialization); plain columns are used directly.
- The **default frame** is materialized by the parser: `RANGE UNBOUNDED PRECEDING
  AND CURRENT ROW` when ORDER BY is present, else `... AND UNBOUNDED FOLLOWING`
  (the whole partition).

Supported functions (qdb v1): `rank`, `sum`, `avg`, `max`.

## CTE (WITH)

A CTE is built once as a canonical definition. Each reference points to that
definition, so reuse remains an optimizer choice instead of being erased by
unconditional build-time inlining. The optional column-alias list renames the
definition's output.

```text
WITH r (k, v) AS (SELECT a, sum(b) FROM t GROUP BY a)
SELECT ... FROM r, r AS other
=> one definition of r, two references
=> optimizer: inline copies or materialize r once for both consumers
```

The CTE phase propagates common predicates and the union of required columns
from all consumers, then chooses inlining versus shared materialization using
estimated cost. Inlined expression trees must be cloned before mutable column
rewrites. `WITH RECURSIVE` is rejected.

### Derived-table column aliases

`( <subquery> ) AS d (c1, c2)` builds the subquery and renames its output to
`(c1, c2)`. These aliases affect column binding, not whether the subplan is
materialized.

## What the builder never does

- extract equi-keys from `ON`/`WHERE` (keys stay empty / in residuals);
- push predicates onto sources or reorder joins;
- attach types or qualify columns.

These are separate, idempotent passes documented alongside.
