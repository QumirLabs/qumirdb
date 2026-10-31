SELECT c, count(*) AS n, sum(p) AS s FROM t WHERE p < 1e30 AND p > -1e30 GROUP BY c ORDER BY c;
