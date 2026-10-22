SELECT count(distinct t.a) AS a, sum((SELECT max(s.v) FROM s WHERE s.k = t.a)) AS m FROM t;
