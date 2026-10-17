SELECT t.id AS id, d.c AS c, d.s AS s, d.n AS n FROM t CROSS JOIN LATERAL (SELECT count(v.v) AS c, sum(v.v) AS s, count(DISTINCT v.v) AS n FROM v WHERE v.k=t.k) AS d;
