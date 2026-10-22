SELECT t.id % 2 AS g, sum(CASE WHEN t.a IN (SELECT s.k FROM s) THEN t.b ELSE 0 END) AS n
FROM t GROUP BY ROLLUP (t.id % 2);
