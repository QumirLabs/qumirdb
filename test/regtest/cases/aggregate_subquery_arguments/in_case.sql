SELECT count(*) AS c, sum(CASE WHEN t.a IN (SELECT s.k FROM s) THEN 1 ELSE 0 END) AS n FROM t;
