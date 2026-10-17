# Subquery decorrelation

Subqueries are removed at **build time**, before the optimizer passes, so the
resulting joins flow through [join_reorder.md](join_reorder.md) and
[predicate_pushdown.md](predicate_pushdown.md) like any other.

## EXISTS / IN → semi / anti join

A top-level `WHERE` conjunct that is `EXISTS`/`IN` (or its negation) wraps the
current node:

| Conjunct | Join |
|---|---|
| `EXISTS (sub)` | `LEFT SEMI` |
| `NOT EXISTS (sub)` | `LEFT ANTI` |
| `x IN (sub)` | `LEFT SEMI` on `x == <sub's one column>` |
| `x NOT IN (sub)` | `LEFT ANTI` on `x == <sub's one column>` |

`EXISTS`: the subquery's `FROM` becomes the right input, its whole `WHERE`
(correlation + local predicates) becomes the join residual. Equijoin later lifts
the correlation equalities into keys and pushes the rest.

```
... AND EXISTS (SELECT * FROM l2 WHERE l2.l_orderkey=l1.l_orderkey
                                   AND l2.l_suppkey<>l1.l_suppkey)
=>
join left_semi  residual (&& (== l2.l_orderkey l1.l_orderkey)
                             (!= l2.l_suppkey  l1.l_suppkey))
├─ <outer>
└─ source l2
=> after equijoin: key [l1.l_orderkey = l2.l_orderkey], residual (!= ...)
```

The table describes QDB's current construction shapes. `NOT IN` is not
generally equivalent to `NOT EXISTS` when NULLs are possible: an ordinary anti
join alone does not provide general NULL-aware membership semantics.

`IN`: the subquery is built as a full plan (must project exactly one column) and
the residual is `operand == <that column>`.

## Scalar subqueries

`ExtractScalarSubqueries` walks a predicate; each scalar `TSubqueryExpr` is
replaced by a reference to a fresh column, and the subquery is attached to the
node. Two cases.

### Uncorrelated → broadcast cross join

The subquery returns one row, so a cross join just appends its value to every
outer row. Its single output column is renamed `__scalar_N__`:

```
... AND c_acctbal > (SELECT avg(c_acctbal) FROM customer WHERE c_acctbal>0)
=>
filter (> c_acctbal __scalar_0__)
└─ join inner                              # 1-row right side
   ├─ <outer>
   └─ project (__scalar_0__ = avg…)        # global aggregate, one row
```

Used by q11 (HAVING), q15, q22.

### Correlated → group-by + LEFT join (Apply elimination)

A correlated scalar subquery references outer columns. It is decorrelated by
grouping the subquery on its correlation columns and `LEFT JOIN`-ing it back.

Detection: `CollectLocalColumns` gathers the columns of the subquery's own FROM
tables. A `WHERE` equality with exactly one **local** and one **outer** ident is
a correlation pair `(outerCol, localCol)`; the rest stay as the subquery's local
filter.

Rewrite of the subquery:
- `GROUP BY` each `localCol`;
- prepend `localCol AS __corr_N_k__` to its SELECT (a fresh, unambiguous name);
- `WHERE` keeps the local predicates; each correlation equality is renamed
  to `localCol = localCol`.

Then `LEFT JOIN` the outer node with the rewritten subquery on
`outerCol == __corr_N_k__`, and rename the original single output to
`__scalar_N__`.

```
SELECT sum(l_extendedprice)/7.0 FROM lineitem, part
WHERE p_partkey=l_partkey AND p_brand=B AND p_container=C
  AND l_quantity < (SELECT 0.2*avg(l_quantity) FROM lineitem
                    WHERE l_partkey = p_partkey)        -- correlated on p_partkey
=>
filter (< l_quantity __scalar_0__)
└─ join left [p_partkey = __corr_0_0__]
   ├─ <lineitem × part with the other WHERE conjuncts>
   └─ project (__corr_0_0__, __scalar_0__)
      └─ aggregate keys=[__corr_0_0__] aggs=[__scalar_0__ = 0.2*avg(l_quantity)]
         └─ source lineitem            -- group by l_partkey, exposed as __corr_0_0__
```

`LEFT` is required: a part with no matching lineitems yields `NULL`, and
`l_quantity < NULL` is unknown → the row is filtered out, matching SQL scalar
semantics. The comparison stays above the join because it spans both sides.

Multiple correlation columns are supported (one group key + one join key each).
The correlated subquery may itself sit inside an `IN` subquery — it is handled
recursively while that `IN`'s body is built (q20: correlation on `ps_partkey` and
`ps_suppkey`).

### Interaction with the optimizer

The decorrelation LEFT join separates the outer WHERE filter from the inner FROM
chain. Reorder threads the filter's equalities **through** the LEFT join so the
inner chain still reorders connectedly; equijoin pushes the preserved-side
conjuncts down and keeps the scalar comparison above the join. See the q2 example
in [join_reorder.md](join_reorder.md).

The renamed equality `localCol = localCol` only rejects NULL keys, which the
join would not match anyway. `SimplifySelfEquality` drops it for a non-nullable
column and turns it into `localCol IS NOT NULL` for a nullable one before
statistics are estimated, so it is not costed as a selective predicate
([optimizations.md](optimizations.md)).

## Dependent tables in FROM

A derived table can reference preceding FROM items, either with explicit
`LATERAL` or QDB's implicit comma-FROM compatibility syntax. Unlike a scalar
subquery, it may produce several columns and any number of rows per outer row.
INNER/CROSS and LEFT dependent joins use the same logical operators and compiled
execution path in native and browser targets.

The builder projects the referenced outer columns into a distinct parameter
domain. The dependent SELECT runs over that domain and its own FROM input,
retaining the parameter columns through projection, DISTINCT and aggregation.
Its predicate stays inside this parameterized input, so non-equality and NULL
predicates keep their ordinary SQL meaning. The result joins back to the original
outer input on null-safe parameter keys. Deduplicating the domain avoids repeating
the computation for equal parameters; joining back restores every original outer
duplicate. Parameter identity treats NULLs as equal without changing SQL equality
inside the dependent predicate.

A global aggregate must return a group even when its parameter has no matching
inner rows. Its input first INNER joins the domain to marked inner rows, applying
the WHERE predicate there. A separate key-only LEFT join from the domain restores
missing parameter groups without requiring a residual outer-join kernel.
Aggregate arguments become NULL for the
unmatched placeholder; COUNT(*) counts the match marker. Thus an empty group has
COUNT zero and SUM NULL, including aggregates over constants or outer columns.
HAVING and scalar expressions run after those states are finalized. An explicit
GROUP BY uses an INNER parameterized input: missing groups produce no rows.

This construction handles plain SELECT, WHERE, projection, DISTINCT, ordinary
GROUP BY and HAVING. Per-parameter ORDER BY/LIMIT/OFFSET, windows, grouping sets,
nested subqueries and local WITH clauses are rejected rather than being evaluated
once for the entire domain. It is a bounded FROM lowering, not a universal
decorrelator. RIGHT/FULL dependent joins are also rejected.
Outer references in ON conditions within the dependent SELECT's FROM tree are
rejected, even when WHERE also references the outer input. Those conditions
belong to their own join boundary: applying them after the domain joins the
completed FROM input can change which rows an outer join preserves. Local ON
conditions remain supported, including inside outer joins.
LEFT dependent JOIN ON accepts equi-keys or deterministic predicates over the
dependent output; other residual outer-join conditions remain unsupported.
Domain reuse requires deterministic inputs. Until the function catalog exposes a
volatility contract, this lowering accepts known deterministic SQL intrinsics and
rejects other function calls, including external functions and `rand`/`irand`.
This check includes the outer input copied into the parameter domain; outer
windows are conservatively rejected too.

## Scalar decorrelation limits

- correlation is detected only from equality conjuncts (`localCol == outerCol`);
- the correlated subquery must reduce to a single value (one non-correlation
  output column);
- `WITH RECURSIVE` and scalar subqueries in the SELECT list are not handled.
