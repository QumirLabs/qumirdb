SELECT t.a AS a, t.c AS c,
    (SELECT v.e FROM v WHERE v.a = t.a ORDER BY v.e LIMIT 1) AS low_e,
    (SELECT v.e FROM v WHERE v.a = t.a ORDER BY v.e DESC LIMIT 1 OFFSET 1) AS second_e,
    (SELECT v.a FROM v WHERE v.a = t.a ORDER BY v.a LIMIT 1) AS tied,
    (SELECT v.e FROM v WHERE v.a = t.a ORDER BY v.e LIMIT 0) AS none
FROM t;
