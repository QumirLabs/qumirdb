SELECT grouping(x) AS g, x, sum(y) AS s FROM t GROUP BY ROLLUP(2) ORDER BY 1, 2;
