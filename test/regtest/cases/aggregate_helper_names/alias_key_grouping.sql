-- GROUPING(sum_0) finds the key named by the alias.
SELECT x + 1 AS sum_0, sum(y) AS s, GROUPING(sum_0) AS g
FROM (SELECT x, y FROM t) d GROUP BY ROLLUP (sum_0);
