-- The window ORDER BY key over the grouped rows is the computed key.
SELECT a + 1 AS g, sum(sum(b)) OVER (ORDER BY a + 1) AS w FROM t GROUP BY a + 1 ORDER BY g;
