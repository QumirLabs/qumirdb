SELECT o.id AS id, d.val AS val FROM t AS o, LATERAL (SELECT i.v FROM v AS i WHERE i.k=o.k) AS d(val);
