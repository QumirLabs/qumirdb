-- The alias and the column are one key; GROUPING(g) reads it.
SELECT a AS g, GROUPING(g) AS gg, sum(b) AS s FROM t GROUP BY ROLLUP (g, a)
ORDER BY g NULLS LAST, gg, s;
