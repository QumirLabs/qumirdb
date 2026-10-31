-- CASE and COALESCE take the common type: REAL absorbs integers, decimals and fractional
-- literals; with DOUBLE PRECISION they are DOUBLE PRECISION.
SELECT id, CASE WHEN id < 5 THEN f ELSE 2.1 END AS lit_branch,
  CASE WHEN id < 3 THEN f WHEN id < 6 THEN 0.1 ELSE k END AS chain,
  CASE WHEN id < 5 THEN f ELSE d END AS double_branch,
  CASE WHEN id < 5 THEN f ELSE x END AS decimal_branch,
  coalesce(f, 1) AS co_int, coalesce(f, 0.5) AS co_lit, coalesce(1.5, f) AS co_lit_first,
  coalesce(f, d) AS co_double, coalesce(f, x) AS co_decimal
FROM (SELECT id, CAST(d AS REAL) AS f, d, k, CAST(e AS DECIMAL(10,2)) AS x FROM t) q
ORDER BY id;
