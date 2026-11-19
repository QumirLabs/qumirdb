SELECT t.a AS a, t.c AS c,
    (SELECT v.e FROM v WHERE v.a = t.a ORDER BY v.e LIMIT 1) AS low_e,
    (SELECT v.e FROM v WHERE v.a = t.a ORDER BY v.e DESC LIMIT 1) AS high_e,
    (SELECT v.e FROM v WHERE v.a = t.a ORDER BY v.e NULLS FIRST LIMIT 1 OFFSET 1) AS second_e,
    (SELECT v.e FROM v WHERE v.a = t.a ORDER BY abs(v.e - 18) LIMIT 1) AS near_e
FROM t;
