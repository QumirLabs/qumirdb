SELECT x, y % 20 AS k, count(*) AS c FROM t GROUP BY 1, 2 ORDER BY x, k;
