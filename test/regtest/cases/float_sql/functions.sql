-- abs(REAL) is REAL, like PostgreSQL's abs(real).
SELECT id, abs(f) AS abs_f, abs(-f) AS abs_neg, abs(f) + abs(g) AS abs_sum, abs(f * 1) AS abs_double
FROM (SELECT id, CAST(d AS REAL) AS f, CAST(e AS REAL) AS g FROM t) q
ORDER BY id;
