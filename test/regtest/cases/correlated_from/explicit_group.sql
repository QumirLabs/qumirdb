SELECT t.id AS id, d.val AS val, d.c AS c FROM t, LATERAL (SELECT v.v AS val, count(*) AS c FROM v WHERE v.k=t.k GROUP BY v.v) AS d;
