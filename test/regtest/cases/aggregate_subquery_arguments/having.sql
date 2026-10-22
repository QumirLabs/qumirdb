SELECT t.id % 2 AS g, count(*) AS c FROM t GROUP BY t.id % 2
HAVING sum(CASE WHEN t.a IN (SELECT s.k FROM s) THEN t.b ELSE 0 END) > 30;
