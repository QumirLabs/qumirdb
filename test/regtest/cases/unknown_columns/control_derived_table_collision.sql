SELECT count(*) AS n
FROM (SELECT id AS k, a AS t1 FROM t)
JOIN (SELECT id AS k, a AS t2 FROM t) ON t1 = t2;
