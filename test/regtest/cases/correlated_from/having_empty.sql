SELECT t.id AS id, d.c AS c FROM t, LATERAL (SELECT count(*) AS c FROM v WHERE v.k=t.k HAVING count(*)=0) AS d;
