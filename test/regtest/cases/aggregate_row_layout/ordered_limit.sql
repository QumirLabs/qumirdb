SELECT CAST(x.id AS DECIMAL(20, 2)) AS d, y.id AS k, x.n AS n,
    count(*) AS c, count(z.a) AS ca, sum(z.a + y.v) AS sa, min(z.a - x.v) AS na,
    max(z.a * y.v) AS xa, sum(x.v * y.v + z.v) AS sv, min(z.f) AS nf, max(y.f + z.f) AS xf
FROM t AS x, t AS y, t AS z
WHERE z.id < 4
GROUP BY CAST(x.id AS DECIMAL(20, 2)), y.id, x.n
ORDER BY sv DESC, d DESC, k
LIMIT 10;
