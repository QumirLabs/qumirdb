-- REAL compares with REAL as REAL and with any other number as DOUBLE PRECISION, so the
-- literal 0.1 does not equal REAL 0.1. NaN rows are left out: comparisons with NaN differ.
SELECT id, f = 0.1 AS lit, f = CAST(0.1 AS REAL) AS real_lit, f < 0.1 AS lt_lit,
  f = 16777217 AS int_lit, f = 16777216 AS int_lit_exact, f = d AS double_col, f = k AS int_col,
  f < x AS decimal_col, f = g AS real_col
FROM (SELECT id, CAST(d AS REAL) AS f, CAST(e AS REAL) AS g, d, k, CAST(e AS DECIMAL(10,2)) AS x FROM t) q
WHERE id <> 5
ORDER BY id;
