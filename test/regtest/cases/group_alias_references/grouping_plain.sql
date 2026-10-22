-- A plain GROUP BY is one grouping set holding every key: GROUPING() is 0.
SELECT a AS g, GROUPING(g) AS gg, sum(b) AS s FROM t GROUP BY g ORDER BY g NULLS LAST;
