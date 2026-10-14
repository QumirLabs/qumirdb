# Predicate pushdown & equi-key extraction

`ExtractEquiJoins` is one pass that does three things over the whole tree:

1. lifts equalities from `WHERE`/`ON` into **join keys** (transitively, via
   equivalence classes);
2. **pushes** the remaining predicates down to the side(s) they constrain;
3. leaves only genuine cross-side predicates as a join **residual**.

Predicate pushdown first runs without key extraction before cardinality
estimation and [join ordering](join_reorder.md). The extraction phase then
runs on the chosen tree, placing keys and repeating safe predicate placement.

## Model

The pass walks top-down carrying a **pool** of conjuncts (`TConjuct`). A conjunct
is either an equality between two column idents (`equiv`, with `left`/`right`
names) or an opaque expression.

- `filter`            → drop the node, add its conjuncts to the pool, recurse.
- `project`/`aggregate` → recurse the input with an **empty** pool (a barrier:
  columns are redefined), then materialize the pool's conjuncts above as a filter.
- `window` → a **partial** barrier: conjuncts over `PARTITION BY` columns only are
  pushed into the child; the rest (including any over the window's output columns)
  materialize above. See [below](#window-partial-barrier).
- redistributable `join` (inner, semi, anti) → `ProcessJoin`.
- outer `join` (left, right, full) → `ProcessOuterJoin`.
- leaf (`source`) → materialize the pool above it as a filter.

Conjuncts are obtained with `FactorConjuncts`, which both flattens `&&` and
**factors atoms common to every branch of an `||`** (see below).

`ColumnsOf(conjunct)` = the columns it references. For an opaque expression this
is `FindUnboundVars`, which excludes a call's callee (a function name is not a
column) so that e.g. `like(o_comment, '…')` counts as constraining only
`o_comment` and can be pushed to one side.

## Window (partial barrier)

`ProcessWindow` (`equijoin.cpp`) recurses into the window's child — so joins and
predicates **below** the window are still optimized — and splits the incoming
pool by coverage:

- a conjunct whose columns are all `PARTITION BY` keys → pushed into the child;
- everything else → materialized as a `filter` above the window.

Pushing a partition-key predicate below the window is always safe: it drops whole
partitions, and a per-partition window result is independent of which other
partitions exist. A predicate over any **non-partition** input column is *not*
safe (it would remove rows inside a partition and change the window result), and
a predicate over a **window output** column (e.g. `WHERE rn = 1` over
`row_number()`) cannot move below the node that computes it — both stay above.

```
filter (&& (= p 5) (= rn 1))          filter (= rn 1)
└─ window partition=[p] ...      =>    └─ window partition=[p] ...
   └─ <input>                             └─ filter (= p 5)
                                             └─ <input>
```

## ProcessJoin (inner / semi / anti)

```
pool += join.residual            # fold ON / decorrelation correlation into pool
classes = union-find over equiv conjuncts in pool
left, right = column sets of each input
```

Per equivalence class, split its members by side and emit **one** cross-side key;
reconnect the remaining same-side members with a spanning tree pushed down so a
deeper join can lift its own key:

```
class {a.x, b.x, c.x}  with a,b on left, c on right
=> join key (a.x, c.x)
   push a.x == b.x to the LEFT input        # deeper join re-extracts it
```

This guarantees one key per class (no redundant duplicates) and propagates
transitive equalities to the level that can use them.

Non-equi conjuncts are partitioned by coverage:

```
covers(cols, leftCols)  → push to left
covers(cols, rightCols) → push to right
otherwise               → residual on the join (+ DeriveSideFilter, below)
```

### Worked example

```
input pool (at the top join of a connected 4-way chain):
  l_orderkey == o_orderkey, l_partkey == ps_partkey, s_suppkey == ps_suppkey,
  o_orderdate < D, like(p_name,'%x%')
```

`o_orderkey` straddles → key; `o_orderdate < D` covers orders → pushed onto
orders; `like(p_name,…)` covers part → pushed onto part; the partsupp equalities
are pushed/keyed at the deeper joins.

## Disjunctions: factoring and DeriveSideFilter

`FactorConjuncts` rewrites `(a∧x) ∨ (a∧y) ≡ a ∧ (x∨y)` so a join equality buried
in an `OR` is exposed (TPC-H q19): the common `p_partkey == l_partkey` is hoisted
to the pool and becomes a key; the reduced `OR` stays as the residual.

For a residual disjunction that still spans both sides, `DeriveSideFilter`
derives a **necessary** single-side condition and pushes it down: for each
disjunct keep only the atoms over one side; if every disjunct constrains that
side, their `OR` is a valid filter for it.

```
(brand=35 ∧ qty∈[10,20]) ∨ (brand=34 ∧ qty∈[18,28])
  left  (part):     brand=35 ∨ brand=34         → pushed to part
  right (lineitem): qty∈[10,20] ∨ qty∈[18,28]   → pushed to lineitem
  full OR stays as the join residual
```

## ProcessOuterJoin (left / right / full)

An outer join's keys and residual together preserve its ON condition.
Residual matching is supported: only candidates passing both keys and residual
count as matches, including for unmatched-row finalization.

- Equi keys are lifted from the `ON` only (WHERE conjuncts from above are *not*
  folded into the ON of an outer join).
- A non-key `ON` conjunct that constrains **only the null-extended side** is
  pushed onto that side *before* the join — equivalent for an outer join:

```
customer LEFT JOIN orders ON c_custkey=o_custkey AND not like(o_comment,'%x%')
=>
join left [c_custkey = o_custkey]
├─ source customer
└─ filter (not like o_comment '%x%')        # pushed to the null side
   └─ source orders
```

  Putting that predicate *above* the join would be wrong: a customer whose only
  orders match the comment would get matched rows, then dropped, instead of
  appearing null-extended with `count = 0`.
- `WHERE` conjuncts from above are pushed only onto the **preserved** side; the
  rest are materialized above the join.

## Invariants after the pass

- inner/semi/anti joins may carry keys and a residual;
- outer joins retain ON residuals that cannot safely move to an input;
- a `filter` remains only where a predicate could not be pushed further down.

## Parquet row-group pruning

After relational rewrites and CTE reuse, `AttachRowGroupPredicates` collects the
complete chain of filters immediately above each source and stores a cloned
predicate on `TSourceOperator`. The filter itself remains in the plan and still
checks every emitted row; the source predicate is only a conservative scan hint.

`BuildPredicateSuperset` is shared with CTE predicate propagation. It groups
conjuncts by their referenced column set. For alternative CTE consumers, a group
is retained only when every consumer constrains those columns, and their
conditions are combined with `OR`. A source path is the single-alternative case,
so its conjuncts are preserved, including expressions over several columns.

During scheduler lowering, a Parquet source compiles the hint once into a Qumir
VM function over an interval domain. Column identifiers receive the row group's
typed `min`, `max`, `null_count`, and NaN possibilities. Comparisons, boolean
operators, and supported arithmetic are evaluated abstractly. Unsupported
syntax, missing or invalid statistics, overflow, and VM failures all mean
"possibly true" and keep the row group. A row group is removed only when `TRUE`
is impossible under SQL `WHERE` semantics.

The frontend context caches parsed `.oz` modules across compilations, while each
compiled function has independent resolver/lowering state. VM evaluation returns
the scalar truth bits directly, without formatting and reparsing a string.
Lowering caches the resulting explicit split list per source. In that cache,
`nullopt` means "use the original source", an engaged non-empty list means the
chosen row groups, and an engaged empty list is a proven empty scan.

The source hint is included in text plans and S-expression round-trips. Optional
diagnostics are carried explicitly through scheduler settings; only the CLI maps
`QDB_DEBUG_ROW_GROUP_PREDICATE` to that option.
