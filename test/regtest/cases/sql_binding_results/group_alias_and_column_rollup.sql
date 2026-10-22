SELECT x AS a, sum(y) AS s, grouping(x) AS g FROM t GROUP BY ROLLUP(a, x) ORDER BY a NULLS LAST, s, g;
