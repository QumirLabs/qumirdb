-- REAL with REAL is REAL; beside any other number REAL is widened to DOUBLE PRECISION.
SELECT id, f + g AS real_real, f / g AS real_div, -f AS neg, f * 2 AS real_int, f - k AS real_int_col,
  f + d AS real_double, f / 4 AS real_div_int, f + 1.5 AS real_lit, f + x AS real_decimal
FROM (SELECT id, CAST(d AS REAL) AS f, CAST(e AS REAL) AS g, d, k, CAST(e AS DECIMAL(10,2)) AS x FROM t) q
ORDER BY id;
