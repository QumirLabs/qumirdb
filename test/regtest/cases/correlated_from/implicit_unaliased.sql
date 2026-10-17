SELECT t.id AS id, col0 AS c FROM t, (SELECT count(*) FROM v WHERE v.k=t.k);
