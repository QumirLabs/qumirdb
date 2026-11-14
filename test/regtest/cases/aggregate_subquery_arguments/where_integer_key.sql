SELECT count(*) AS c,
    sum((SELECT min(s.v) FROM s WHERE s.k = t.a)) AS lo,
    sum(CASE WHEN t.a IN (SELECT s.k FROM s) THEN t.b ELSE 0 END) AS n
FROM t
WHERE t.b <= (SELECT max(s.v) FROM s WHERE s.k = t.a);
