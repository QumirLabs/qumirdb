SELECT count(*) AS groups, sum(c) AS row_count, sum(d) AS sum_d, sum(k) AS sum_k, sum(n) AS sum_n,
    count(n) AS count_n, sum(ca) AS count_a, sum(sa) AS sum_a, min(na) AS min_a, max(xa) AS max_a,
    sum(sv) AS sum_v, min(nf) AS min_f, max(xf) AS max_f, sum(sf) AS sum_f
FROM (
    SELECT CAST(x.id AS DECIMAL(20, 2)) AS d, y.id AS k, x.n AS n,
        count(*) AS c, count(z.a) AS ca, sum(z.a + y.v) AS sa, min(z.a - x.v) AS na,
        max(z.a * y.v) AS xa, sum(x.v * y.v + z.v) AS sv, min(z.f) AS nf, max(y.f + z.f) AS xf,
        sum(z.f) AS sf
    FROM t AS x, t AS y, t AS z
    WHERE z.id < 4
    GROUP BY CAST(x.id AS DECIMAL(20, 2)), y.id, x.n
) AS g;
