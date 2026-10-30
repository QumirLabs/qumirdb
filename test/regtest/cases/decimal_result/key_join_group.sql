SELECT x.k, count(*) AS c, sum(y.id) AS s
FROM (SELECT id, CAST(b AS DECIMAL(38,6)) AS k FROM t) x
LEFT JOIN (SELECT id, CAST(b AS DECIMAL(38,6)) AS k FROM t) y ON x.k = y.k
GROUP BY x.k ORDER BY x.k;
