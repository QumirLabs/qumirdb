SELECT count(DISTINCT f) AS distinct_f, count(f) AS c
FROM (SELECT CAST(d AS REAL) AS f FROM t) q;
