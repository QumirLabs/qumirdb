SELECT t.id AS id, d.c AS c FROM t, (SELECT count(*) AS c FROM v) AS d;
