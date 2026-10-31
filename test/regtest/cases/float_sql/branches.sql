-- CASE and COALESCE take the common type: REAL absorbs integers, decimals and fractional
-- literals; with DOUBLE PRECISION they are DOUBLE PRECISION. PostgreSQL types arithmetic on
-- integers, decimals and literals as NUMERIC, so REAL absorbs it; REAL or DOUBLE arithmetic stays DOUBLE.
SELECT id, CASE WHEN id < 5 THEN f ELSE 2.1 END AS lit_branch,
  CASE WHEN id < 3 THEN f WHEN id < 6 THEN 0.1 ELSE k END AS chain,
  CASE WHEN id < 5 THEN f ELSE d END AS double_branch,
  CASE WHEN id < 5 THEN f ELSE x END AS decimal_branch,
  coalesce(f, 1) AS co_int, coalesce(f, 0.5) AS co_lit, coalesce(1.5, f) AS co_lit_first,
  coalesce(f, d) AS co_double, coalesce(f, x) AS co_decimal,
  CASE WHEN id < 5 THEN f ELSE 0.1 + 0.2 END AS lit_expr,
  CASE WHEN id < 5 THEN f ELSE k * 0.1 END AS col_expr,
  CASE WHEN id < 5 THEN f ELSE -(k * 0.1) END AS neg_expr,
  coalesce(f, 0.1 + 0.2) AS co_lit_expr, coalesce(f, k * 0.1) AS co_col_expr,
  CASE WHEN id < 5 THEN f ELSE f + 0.1 END AS real_expr,
  CASE WHEN id < 5 THEN f ELSE d * 0.1 END AS double_expr
FROM (SELECT id, CAST(d AS REAL) AS f, d, k, CAST(e AS DECIMAL(10,2)) AS x FROM t) q
ORDER BY id;
