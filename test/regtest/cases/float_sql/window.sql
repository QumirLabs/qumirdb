-- Window sum and max of REAL are REAL, avg is DOUBLE PRECISION.
SELECT id, sum(g) OVER (ORDER BY id) AS run_sum, max(g) OVER (ORDER BY id) AS run_max,
  avg(g) OVER (PARTITION BY id % 2) AS part_avg, sum(g) OVER (PARTITION BY id % 2) AS part_sum
FROM (SELECT id, CAST(e AS REAL) AS g FROM t WHERE id > 1) q
ORDER BY id;
