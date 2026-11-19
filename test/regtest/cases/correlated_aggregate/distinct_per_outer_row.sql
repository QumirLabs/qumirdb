SELECT t.a AS a, t.c AS c,
    (SELECT DISTINCT v.a FROM v WHERE v.a = t.a) AS k,
    (SELECT DISTINCT 1 FROM v WHERE v.a = t.a) AS found,
    (SELECT DISTINCT a + 1 FROM v WHERE v.a = t.a LIMIT 2) AS next_k
FROM t;
