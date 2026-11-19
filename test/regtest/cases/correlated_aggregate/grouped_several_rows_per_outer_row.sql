SELECT t.a AS a, (SELECT v.e FROM v WHERE v.a = t.a GROUP BY v.e ORDER BY v.e LIMIT 2) AS e FROM t;
