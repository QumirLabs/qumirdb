SELECT t.id AS id, (SELECT COUNT(DISTINCT v.v) + COUNT(*) FROM v WHERE v.k = t.k) AS n FROM t;
