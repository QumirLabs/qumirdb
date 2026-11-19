SELECT t.a AS a, (SELECT v.e FROM v WHERE v.a = t.a ORDER BY v.e LIMIT 2) AS e FROM t;
