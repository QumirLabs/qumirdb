SELECT t.id AS id, (SELECT COUNT(*) FROM v WHERE v.k = t.k HAVING COUNT(*) > 0) AS n FROM t;
