SELECT t.id AS id, (SELECT COUNT(*) + 1 FROM v WHERE v.k = t.k) AS n FROM t;
