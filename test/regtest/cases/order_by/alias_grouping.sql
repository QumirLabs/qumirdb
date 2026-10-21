SELECT a AS a, grouping(a) AS g, count(*) AS c FROM t
GROUP BY ROLLUP(a)
ORDER BY g DESC, CASE WHEN g = 0 THEN a END NULLS FIRST, c;
