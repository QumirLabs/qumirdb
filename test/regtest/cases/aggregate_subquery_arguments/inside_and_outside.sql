SELECT sum(CASE WHEN t.a IN (SELECT s.k FROM s) THEN t.b ELSE 0 END) + (SELECT max(s.v) FROM s) AS n
FROM t;
