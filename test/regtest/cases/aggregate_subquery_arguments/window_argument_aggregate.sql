SELECT t.id,
    sum(sum(CASE WHEN t.a IN (SELECT s.k FROM s) THEN t.b ELSE 0 END)) OVER (ORDER BY t.id) AS running
FROM t GROUP BY t.id;
