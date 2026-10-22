SELECT t.id % 2 AS g, sum((SELECT max(s.v) FROM s WHERE s.k = t.a)) AS m
FROM t GROUP BY t.id % 2;
