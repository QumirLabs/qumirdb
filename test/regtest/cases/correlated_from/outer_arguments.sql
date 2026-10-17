SELECT t.id AS id, d.c AS c, d.s AS s FROM t, LATERAL (SELECT count(t.tag) AS c, sum(t.tag) AS s FROM v WHERE v.k=t.k) AS d;
