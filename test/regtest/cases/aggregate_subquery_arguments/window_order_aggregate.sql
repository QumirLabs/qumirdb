SELECT t.id,
    rank() OVER (ORDER BY sum(CASE WHEN t.a IN (SELECT s.k FROM s) THEN t.b ELSE 0 END) DESC) AS r
FROM t GROUP BY t.id;
