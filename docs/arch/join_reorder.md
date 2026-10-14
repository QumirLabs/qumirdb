# Join reorder

`ReorderJoins` rewrites each left-deep chain of cross joins (the comma-join shape
the builder emits) into a **connected** order, so that adjacent joins always have
a real equality between them. Only genuinely disconnected relations stay
cross-joined. Predicate pushdown and input estimation run before ordering; equi-key
extraction follows it. Reorder fixes the tree *shape*, then extraction lifts
keys onto that tree (see [predicate_pushdown.md](predicate_pushdown.md)).

## Pipeline position

Runs after column qualification, typing, predicate pushdown, and input
estimation, and before `ExtractEquiJoins`. With CBO enabled, inner-join
flattening first exposes reassociation opportunities. Types are re-annotated
after ordering because the tree is rebuilt.

## Cost-based order and fallback

With input row counts and positive NDVs on both endpoints of every join edge,
QDB searches connected relation subsets for the cheapest bushy tree under its
cost model. Each larger candidate combines two disjoint connected subplans
linked by an equality. It retains the cheapest candidate for that subset:

```text
best(A union B) = min over connected splits:
    cost(best(A)) + cost(best(B)) + estimated join work(A, B)
```

This is the connected-subgraph/complement search space described by Moerkotte
and Neumann in [Analysis of Two Existing and One New Dynamic Programming
Algorithm for the Generation of Optimal Bushy Join Trees without Cross Products
(VLDB 2006)](https://www.vldb.org/conf/2006/p930-moerkotte.pdf).
QDB uses a bounded subset search over that space, rather than the paper's full
specialized pair-enumeration procedure. Disconnected components are planned
separately and then cross-joined.

The greedy connected-order algorithm below remains the fallback when CBO is
disabled, rows or key NDVs are missing, there are no equality edges, a component
exceeds the current 12-relation bound, or search produces no tree. Its examples
illustrate connectivity; the cost-based path can choose a different tree.

## Why it is needed

The builder emits the FROM order verbatim. `FROM part, supplier, lineitem` is
left-deep `((part × supplier) × lineitem)`, but `part` and `supplier` share no
predicate — they only connect *through* `lineitem`. Equijoin extracts keys on the
given shape; it cannot invent a join order, so `part × supplier` would stay a
real cross product.

## Algorithm

The greedy fallback has two phases over each reorderable chain.

A join is **reorderable** iff it is `inner`, has empty keys, and has no residual
(the comma-join shape, also exposed by CBO inner-join flattening;
semi/anti and outer joins are opaque).

### 1. Collect edges and leaves

`Reorder(node, edges)` threads equality edges down from enclosing filters:

- `filter`     → add the predicate's `col == col` equalities to `edges`, recurse.
- `join` (any) → pass `edges` to both children.
- `project`/`aggregate` → recurse with **empty** edges (columns are redefined).
- reorderable chain → flatten to its leaf operators, reorder each leaf with empty
  edges, then rebuild (phase 2).

Edges are collected with `FactorConjuncts` (so an equality factored out of a
disjunction also counts) and keep both operand names. Because names are
qualified, an edge from one scope never matches leaves of another.

### 2. Greedy connected order (`BuildChain`)

Map each output column to its owning leaf (via `OutputColumns`). Each edge whose
both endpoints are owned by distinct leaves adds an adjacency. Then:

```
order = []
while some leaf unplaced:
    seed = first unplaced leaf            # starts a new component (a cross)
    place seed
    repeat: place any unplaced leaf adjacent to an already-placed one
rebuild left-deep: ((order[0] × order[1]) × order[2]) ...   # inner, empty keys
```

Each appended leaf shares an edge with the already-placed side, so the join that
attaches it has a straddling equality → equijoin lifts a key there. Components
that share no edge are concatenated (an unavoidable cross).

## Examples

### Comma join (q9)

```
FROM part, supplier, lineitem, partsupp, orders, nation
WHERE p_partkey=l_partkey AND s_suppkey=l_suppkey AND ps_suppkey=l_suppkey
  AND ps_partkey=l_partkey AND o_orderkey=l_orderkey AND s_nationkey=n_nationkey
```

Edges: part–lineitem, supplier–lineitem, partsupp–lineitem, orders–lineitem,
supplier–nation. Greedy from `part`:

```
part, lineitem, supplier, partsupp, orders, nation
=> ((((part × lineitem) × supplier) × partsupp) × orders) × nation
```

Every join now has a direct equality; equijoin lifts a key on each. `supplier`
attaches via `s_suppkey=l_suppkey`, `nation` via `s_nationkey=n_nationkey`, etc.

### Chain under a decorrelation LEFT JOIN (q2)

Scalar decorrelation wraps the outer query in a `LEFT JOIN`, leaving the WHERE
filter *above* it and the inner chain *under* its left input:

```
filter (... AND ps_supplycost == __scalar_0__)         -- holds the chain's edges
└─ join left [p_partkey = __corr_0_0__]
   ├─ <inner chain: part, supplier, partsupp, nation, region>   (cross)
   └─ <decorrelated subquery>
```

The filter's edges (`p_partkey=ps_partkey`, `s_suppkey=ps_suppkey`,
`s_nationkey=n_nationkey`, `n_regionkey=r_regionkey`) are threaded **through** the
LEFT join into the inner chain. `ps_supplycost == __scalar_0__` is also an edge
but `__scalar_0__` is not a chain leaf, so it is ignored when ordering the chain.
Result:

```
((((part × partsupp) × supplier) × nation) × region)
```

The subquery on the right side is reordered independently using its own filter's
edges (edges reset across the project/aggregate above it).

## Non-goals

The greedy fallback is connectivity-driven and does not compare costs. The CBO
path chooses among connected bushy trees using estimates; neither path performs
unrestricted reassociation across outer/semi/anti joins.
