-- A subquery in the projection of a computed key reads the grouped rows.
SELECT a + 1 AS g, (SELECT max(x) FROM u) AS m FROM t GROUP BY a + 1 ORDER BY g;
