-- PostgreSQL has no round(real, int) and no real % integer; qdb keeps them and widens REAL to
-- DOUBLE PRECISION, as beside any other number. round(real) is round(double precision).
SELECT id, round(f) AS round_f, round(f, 2) AS round_2, round(f, -1) AS round_neg1,
  f % 2 AS mod_int, f % 0.5 AS mod_lit, f % CAST(g AS REAL) AS mod_real
FROM (SELECT id, CAST(d AS REAL) AS f, CAST(e AS REAL) AS g FROM t) q
ORDER BY id;
