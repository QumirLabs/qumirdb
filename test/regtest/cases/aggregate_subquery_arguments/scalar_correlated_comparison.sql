SELECT sum(CASE WHEN t.b > (SELECT avg(s.v) FROM s WHERE s.k = t.a) THEN 1 ELSE 0 END) AS n FROM t;
