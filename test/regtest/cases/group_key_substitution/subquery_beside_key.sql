-- The key beside a subquery in one expression reads the key column.
SELECT a + 1 AS g, (SELECT max(x) FROM u) - (a + 1) AS d
FROM t GROUP BY a + 1 ORDER BY g;
