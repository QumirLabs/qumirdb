SELECT t.id AS id, (SELECT SUM(v.v) FROM v WHERE v.k = t.k) AS s, (SELECT MIN(v.v) FROM v WHERE v.k = t.k) AS mn, (SELECT AVG(v.v) FROM v WHERE v.k = t.k) AS a FROM t;
