SELECT x + 1 AS a, sum(y) AS s FROM t GROUP BY 1 ORDER BY a;
