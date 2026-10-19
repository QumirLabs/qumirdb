SELECT t.id AS id, d.val AS val FROM t, LATERAL (SELECT v.v AS val FROM v WHERE v.k=t.k) AS d;
