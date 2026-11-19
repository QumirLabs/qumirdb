SELECT t.id AS id, (SELECT COUNT(v.v) FROM v WHERE v.k = t.k) AS n FROM t;
