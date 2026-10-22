SELECT x, sum(y) AS s, grouping(x) AS g FROM t GROUP BY ROLLUP(1, x) ORDER BY x NULLS LAST, s, g;
