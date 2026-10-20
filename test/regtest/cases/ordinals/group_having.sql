SELECT x, sum(y) AS s FROM t GROUP BY 1 HAVING sum(y) > 40 ORDER BY x;
