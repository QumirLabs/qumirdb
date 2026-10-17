SELECT t.id AS id, d.c AS c FROM (SELECT * FROM t WHERE id<0) AS t, LATERAL (SELECT count(*) AS c FROM v WHERE v.k=t.k) AS d;
