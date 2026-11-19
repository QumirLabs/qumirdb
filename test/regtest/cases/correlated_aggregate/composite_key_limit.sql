SELECT t.a AS a, t.b AS b,
    (SELECT v.e FROM v WHERE v.a = t.a AND v.b = t.b ORDER BY v.e LIMIT 1) AS low_e,
    (SELECT v.e FROM v WHERE v.a = t.a AND v.b = t.b ORDER BY v.e DESC LIMIT 1) AS high_e
FROM t;
