# Subquery decorrelation

Subqueries are removed at **build time**, before the optimizer passes, so the
resulting joins flow through [join_reorder.md](join_reorder.md) and
[predicate_pushdown.md](predicate_pushdown.md) like any other.

## Local and outer references

Rewrites that need a subquery's correlation - a scalar subquery, an `IN` nested in
a boolean expression, a disjunction of `EXISTS` - resolve each identifier in the
subquery's `WHERE` against the subquery's own `FROM` first, as SQL scoping does. A
qualified reference is local when its qualifier names a `FROM` item. A bare
reference is local when a `FROM` item - a table, a CTE or a derived table - has a
column of that name, so an inner column shadows an outer one. Every other
reference is outer. Table names resolve as in the plan itself: the subquery's own
CTEs shadow those of enclosing queries and the base tables. The columns of every
`FROM` item are known before the subquery is built, from the table schema or the
select list of a CTE or derived table, so no reference is left unclassified.

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

Detection: a `WHERE` equality between one **local** and one **outer** ident (see
[Local and outer references](#local-and-outer-references)) is a correlation pair
`(outerCol, localCol)` when every row passing the `WHERE` satisfies it: it is a
conjunct, or every branch of an `OR` requires it, as in
`(a = out AND P1) OR (a = out AND P2)`. The rest stay as the subquery's local
filter. An equality inside only some branches of an `OR`, or under `NOT`, holds
just for some rows: as a group and join key it would drop the rows of the other
branches, so such a subquery is rejected with an error until general unnesting
handles it. The same rule picks the correlation of each `EXISTS` in a disjunction
of `EXISTS`.

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

## Limits

- correlation is detected only from equalities (`localCol == outerCol`) that the
  whole predicate requires;
- the correlated subquery must reduce to a single value (one non-correlation
  output column);
- `WITH RECURSIVE` and scalar subqueries in the SELECT list are not handled.
