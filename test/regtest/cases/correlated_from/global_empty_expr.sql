SELECT t.id AS id, d.n AS n, d.s AS s FROM t, LATERAL (SELECT count(*)+1 AS n, sum(1) AS s FROM v WHERE v.k=t.k AND v.v>1000) AS d;
