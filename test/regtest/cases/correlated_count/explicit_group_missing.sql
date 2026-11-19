SELECT t.id AS id, (SELECT COUNT(*) FROM v WHERE v.k = t.k GROUP BY v.k) AS n FROM t;
