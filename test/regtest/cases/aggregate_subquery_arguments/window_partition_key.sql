SELECT t.id, sum(t.b) OVER (PARTITION BY t.a IN (SELECT s.k FROM s)) AS n FROM t;
