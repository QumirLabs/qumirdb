-- GROUPING(g) finds the key of the column the alias g names.
SELECT a AS g, GROUPING(g) AS gg, sum(b) AS s FROM t GROUP BY ROLLUP (g)
ORDER BY g NULLS LAST, gg;
