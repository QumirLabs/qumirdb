SELECT count(*) AS c,
    count((SELECT max(s.v) FROM s WHERE s.k = t.a)) AS matched,
    sum((SELECT max(s.v) FROM s WHERE s.k = t.a)) AS total
FROM t;
