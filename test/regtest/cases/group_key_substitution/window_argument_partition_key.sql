-- The window argument and PARTITION BY over the grouped rows read the computed key.
SELECT a + 1 AS g, sum(a + 1) OVER (PARTITION BY a + 1 > 1) AS w
FROM t GROUP BY a + 1 ORDER BY g;
