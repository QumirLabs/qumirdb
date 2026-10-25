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
flattening first exposes reassociation opportunities; outer-join simplification
runs before it, so an outer join that a predicate above makes inner joins the
region ([optimizations.md](optimizations.md)). Types are re-annotated after
ordering because the tree is rebuilt.

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

### Chain under an outer join (q72)

A `WHERE` filter above an outer join can hold edges of the inner chain under the
join's preserved input. TPC-DS q72 joins nine relations with `JOIN ... ON`,
then LEFT-joins `promotion` and `catalog_returns`; its `WHERE` compares
`d1.d_week_seq = d2.d_week_seq`. No `WHERE` conjunct rejects the NULLs of the
two outer inputs, so both joins stay LEFT:

```
filter (d1.d_week_seq == d2.d_week_seq AND ...)        -- holds a chain edge
└─ join left (cs_item_sk == cr_item_sk AND cs_order_number == cr_order_number)
   ├─ join left (cs_promo_sk == p_promo_sk)
   │  ├─ <inner chain: catalog_sales, inventory, warehouse, item, ..., d1, d2, d3>
   │  └─ promotion
   └─ catalog_returns
```

The edge `d1.d_week_seq = d2.d_week_seq` is threaded **through** both LEFT joins
into the inner chain. Ordering uses it as an edge between `d1` and `d2`, and
equi-key extraction lifts it into a key of the join that brings in `d1`.

When a `WHERE` conjunct does reject the NULLs of an outer input, as the
comparison with a scalar subquery value does after decorrelation (TPC-H q2),
outer-join simplification has already made that join inner, and its input is one
more relation of the region.

## Non-goals

The greedy fallback is connectivity-driven and does not compare costs. The CBO
path chooses among connected bushy trees using estimates; neither path performs
unrestricted reassociation across outer/semi/anti joins.
