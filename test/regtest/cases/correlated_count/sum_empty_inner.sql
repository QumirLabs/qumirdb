SELECT t.id AS id, (SELECT SUM(v.v) FROM v WHERE v.k = t.k) AS n FROM t;
