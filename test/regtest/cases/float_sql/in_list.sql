-- Two or more constants compare in the common type of the list and its left operand (REAL
-- here); one constant and a column item each compare as `=`.
SELECT id, f IN (0.1) AS one_lit, f IN (0.1, 0.2) AS two_lits, f IN (16777217, 3) AS two_ints,
  f IN (CAST(0.1 AS DOUBLE PRECISION), 2.5) AS with_double, f IN (g, 1) AS with_column, k IN (f, 7) AS int_lhs
FROM (SELECT id, CAST(d AS REAL) AS f, CAST(e AS REAL) AS g, k FROM t) q
WHERE id <> 5
ORDER BY id;
