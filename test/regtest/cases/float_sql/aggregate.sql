-- sum, min and max of REAL are REAL; avg and stddev_samp are DOUBLE PRECISION. The values
-- add exactly in REAL, where PostgreSQL accumulates sum.
SELECT count(g) AS c, sum(g) AS s, avg(g) AS a, min(g) AS lo, max(g) AS hi, stddev_samp(g) AS sd
FROM (SELECT CAST(e AS REAL) AS g FROM t WHERE id > 1) q;
