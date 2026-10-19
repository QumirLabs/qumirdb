SELECT t.id AS id, d.v AS val FROM t, LATERAL (SELECT * FROM v WHERE v.k=t.k) AS d;
