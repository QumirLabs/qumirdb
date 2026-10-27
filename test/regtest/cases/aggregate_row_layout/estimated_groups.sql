SELECT count(*) AS groups, sum(c) AS row_count, sum(i) AS sum_i, sum(k) AS sum_k, sum(sa) AS sum_a,
    min(nf) AS min_f, max(xv) AS max_v
FROM (
    SELECT x.id AS i, y.id AS k, count(*) AS c, sum(z.a) AS sa, min(z.f) AS nf, max(x.v + z.v) AS xv
    FROM t AS x, t AS y, t AS z
    WHERE z.id < 4
    GROUP BY x.id, y.id
) AS g;
